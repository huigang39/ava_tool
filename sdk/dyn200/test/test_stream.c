/* Exercise the real SDK read/table state machine with an in-memory transport.
 * No serial enumeration, physical port, driver configuration or sensor writes. */
#define DYN200_TEST
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static DWORD test_event_mask;
static unsigned test_mask_calls;
static int test_fail_mask, test_fail_timeout;
static BOOL test_set_comm_mask(HANDLE port, DWORD mask)
{
        (void)port;
        ++test_mask_calls;
        if (test_fail_mask) return FALSE;
        test_event_mask = mask;
        return TRUE;
}
static BOOL test_set_comm_timeouts(HANDLE port, LPCOMMTIMEOUTS timeouts)
{
        (void)port; (void)timeouts;
        return !test_fail_timeout;
}
#define SetCommMask test_set_comm_mask
#define SetCommTimeouts test_set_comm_timeouts
#endif
#include "../src/dyn200.c"
#include <assert.h>

static uint8_t input[65536];
static size_t input_size, input_pos, chunk_limit;
static unsigned read_calls;
static uint64_t test_clock_us;
static int test_open, fail_read;

static int dyn200_test_port_is_open(void) { return test_open; }
static size_t dyn200_test_bytes_available(void) { return input_size - input_pos; }
static uint64_t dyn200_test_monotonic_us(void) { return test_clock_us; }
static int dyn200_test_read_bytes(uint8_t *data, size_t size, size_t *got, uint32_t timeout)
{
        ++read_calls;
        if (fail_read)
                return set_error(DYN200_ERR_READ, "simulated disconnect");
        *got = input_size - input_pos;
        if (*got > size) *got = size;
        if (chunk_limit && *got > chunk_limit) *got = chunk_limit;
        memcpy(data, input + input_pos, *got);
        input_pos += *got;
        if (*got == 0) test_clock_us += (uint64_t)timeout * 1000u;
        return DYN200_OK;
}

static void reset_stream(int frame_size)
{
        dyn200_close();
        input_size = input_pos = chunk_limit = read_calls = 0;
        test_clock_us = 1000000u;
        test_open = 1;
        fail_read = 0;
        g_protocol = frame_size;
        g_timeout_ms = 20;
}

static void append_frame(int frame_size, unsigned val)
{
        uint8_t *frame = input + input_size;
        assert(input_size + (size_t)frame_size <= sizeof(input));
        memset(frame, 0, (size_t)frame_size);
        if (frame_size == 6) {
                frame[0] = (uint8_t)(val >> 8);
                frame[1] = (uint8_t)val;
                frame[3] = 100;
        } else {
                frame[0] = (uint8_t)(val >> 16);
                frame[1] = (uint8_t)(val >> 8);
                frame[2] = (uint8_t)val;
                frame[5] = 100;
        }
        append_crc(frame, (size_t)frame_size - 2u);
        input_size += (size_t)frame_size;
}

static void test_torque_scaling(int frame_size)
{
        dyn200_param param = {0};
        dyn200_rt rt = {0};
        reset_stream(frame_size);
        assert(dyn200_read_param(&param) == DYN200_OK);
        assert(param.scaling.torque_decimals == 1);
        append_frame(frame_size, 83);
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        assert(rt.measurement.torque_raw == 83);
        assert(fabs(rt.measurement.torque_nm - 8.3) < 1e-9);
        for (unsigned decimals = 1; decimals <= 2; ++decimals) {
                param.scaling.torque_decimals = decimals;
                assert(dyn200_write_param(&param) == DYN200_OK);
                assert(param.scaling.torque_decimals == decimals);
                append_frame(frame_size, frame_size == 6 ? 83u : 0xffffadu);
                if (frame_size == 6) {
                        uint8_t *frame = input + input_size - 6;
                        frame[2] |= 0x80;       // Six-byte frames carry torque sign in the speed word
                        append_crc(frame, 4);
                }
                assert(dyn200_read_rt(&rt) == DYN200_OK);
                assert(rt.measurement.torque_raw == -83);
                const double torque = decimals == 1 ? -8.3 : -0.83;
                assert(fabs(rt.measurement.torque_nm - torque) < 1e-9);
                assert(fabs(dyn200_get_torque_nm() - torque) < 1e-9);
                assert(fabs(rt.measurement.power_kw - torque * 100 * 0.00010471975511965977) < 1e-9);
        }
}

static void test_batched_order(int frame_size)
{
        dyn200_rt rt = {0};
        reset_stream(frame_size);
        rt.wait_fresh = 1;
        for (unsigned i = 0; i < 4000; ++i) append_frame(frame_size, i);
        for (unsigned i = 0; i < 4000; ++i) {
                assert(dyn200_read_rt(&rt) == DYN200_OK);
                assert(rt.fresh == 1 && rt.sample_count == (uint64_t)i + 1u);
                assert(rt.measurement.torque_raw == (int32_t)i);
        }
        // A USB-sized batch is drained in blocks, without one transport read per frame
        assert(read_calls == 2);
        assert(dyn200_read_rt(&rt) == DYN200_ERR_TIMEOUT);
        assert(rt.fresh == 0 && rt.sample_count == 4000);
        assert(rt.measurement.torque_raw == 3999);
}

static void test_noise_and_split_frames(int frame_size)
{
        dyn200_rt rt = {0};
        reset_stream(frame_size);
        rt.wait_fresh = 1;
        // More noise than the SDK queue used to fill it forever
        memset(input, 0xa5, 20000);
        input_size = 20000;
        append_frame(frame_size, 1234);
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        assert(rt.measurement.torque_raw == 1234);

        reset_stream(frame_size);
        memset(&rt, 0, sizeof(rt));
        rt.wait_fresh = 1;
        chunk_limit = 1;
        append_frame(frame_size, 4321);
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        assert(rt.measurement.torque_raw == 4321);
        assert(read_calls == (unsigned)frame_size);
}

static void test_latest_and_resume(int frame_size)
{
        dyn200_rt rt = {0};
        reset_stream(frame_size);
        append_frame(frame_size, 1);
        assert(dyn200_read_rt(&rt) == DYN200_OK && rt.fresh == 1);

        // More than one internal buffer: display must reach the newest complete frame
        for (unsigned i = 2; i <= 4000; ++i) append_frame(frame_size, i);
        append_frame(frame_size, 4001);
        input_size -= 2; // Last two bytes have not arrived yet
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        assert(rt.measurement.torque_raw == 4000 && rt.sample_count == 2);
        input_size += 2;
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        assert(rt.measurement.torque_raw == 4001 && rt.sample_count == 3);
        assert(dyn200_read_rt(&rt) == DYN200_OK && rt.fresh == 0);
        assert(rt.sample_count == 3);

        test_clock_us += 1600000u;
        assert(dyn200_read_rt(&rt) == DYN200_ERR_TIMEOUT && rt.fresh == 0);
        append_frame(frame_size, 4321);
        assert(dyn200_read_rt(&rt) == DYN200_OK && rt.fresh == 1);
        assert(rt.measurement.torque_raw == 4321 && rt.sample_count == 4);

        append_frame(frame_size, 4322);
        fail_read = 1;
        assert(dyn200_read_rt(&rt) == DYN200_ERR_READ && rt.fresh == 0);
        assert(rt.measurement.torque_raw == 4321 && rt.sample_count == 4);
}

static void test_disconnected_fresh(void)
{
        dyn200_rt rt = {0};
        reset_stream(6);
        append_frame(6, 12);
        assert(dyn200_read_rt(&rt) == DYN200_OK && rt.fresh == 1);
        dyn200_close();
        test_open = 0;
        assert(dyn200_read_rt(&rt) == DYN200_ERR_NOT_OPEN && rt.fresh == 0);
        assert(rt.measurement.torque_raw == 12 && rt.sample_count == 1);
}

static void test_connection_selection(void)
{
        dyn200_port_param port = {0};
        reset_stream(6);
        set_text(g_port_name, sizeof(g_port_name), "COM_DYN200_TEST_OLD");
        set_text(port.port, sizeof(port.port), "COM_DYN200_MISSING");
        normalize_port_unlocked(&port);
        // An already-open old connection must not turn a requested port switch into success
        assert(connect_selected_unlocked(&port, 1) == DYN200_ERR_OPEN);

        remember_link_unlocked("COM_DYN200_MISSING", 8, 115200, 1, 2, 200);
        remember_link_unlocked(g_link_port, 8, 115200, 1, 2, 200);
        assert(strcmp(g_link_port, "COM_DYN200_MISSING") == 0);
        assert(g_link_protocol == 8 && g_link_baud == 115200);
}

static void test_new_table_and_clock_wrap(void)
{
        dyn200_rt rt = {0};
        reset_stream(6);
        append_frame(6, 42);
        assert(dyn200_update() == DYN200_OK);
        // Another caller's cached sample must not make a new zeroed table look valid
        assert(dyn200_read_rt(&rt) == DYN200_ERR_TIMEOUT && rt.sample_count == 0);

        test_clock_us = ((uint64_t)UINT32_MAX - 10u) * 1000u;
        append_frame(6, 43);
        assert(dyn200_read_rt(&rt) == DYN200_OK);
        test_clock_us += 1600000u; // Cross the 32-bit millisecond rollover
        assert(dyn200_read_rt(&rt) == DYN200_ERR_TIMEOUT && rt.fresh == 0);
        append_frame(6, 44);
        assert(dyn200_read_rt(&rt) == DYN200_OK && rt.measurement.torque_raw == 44);
}

#if defined(_WIN32)
static void test_active_delivery_configuration(void)
{
        test_event_mask = test_mask_calls = 0;
        assert(configure_active_stream(200) == DYN200_OK);
        assert(test_event_mask == EV_RXFLAG && test_mask_calls == 1);
        // A shrinking read deadline must not turn off USB event delivery
        assert(configure_active_read_timeout(75) == DYN200_OK);
        assert(test_event_mask == EV_RXFLAG && test_mask_calls == 1);
        test_fail_mask = 1;
        assert(configure_active_stream(200) == DYN200_ERR_CONFIGURE);
        test_fail_mask = 0;
        test_fail_timeout = 1;
        assert(configure_active_stream(200) == DYN200_ERR_CONFIGURE);
        assert(test_mask_calls == 2);
        test_fail_timeout = 0;
        // A subsequent successful connection re-enables delivery
        test_event_mask = 0;
        assert(configure_active_stream(200) == DYN200_OK);
        assert(test_event_mask == EV_RXFLAG && test_mask_calls == 3);
}
#endif

int main(void)
{
        assert(g_torque_decimals == 1);
#if defined(_WIN32)
        test_active_delivery_configuration();
#endif
        for (int frame_size = 6; frame_size <= 8; frame_size += 2) {
                test_torque_scaling(frame_size);
                test_batched_order(frame_size);
                test_noise_and_split_frames(frame_size);
                test_latest_and_resume(frame_size);
        }
        test_disconnected_fresh();
        test_connection_selection();
        test_new_table_and_clock_wrap();
        puts("stream tests passed (both protocols: bursts, noise, partial frames, stale recovery, errors)");
        return 0;
}
