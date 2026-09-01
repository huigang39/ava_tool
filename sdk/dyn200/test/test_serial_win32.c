// No handles or hardware: reject dependence on another application's DCB
#define WIN32_LEAN_AND_MEAN
#include "../src/dyn200_serial_win32.h"

#include <assert.h>
#include <stdio.h>

static void assert_binary_no_flow(const DCB *dcb, DWORD baud, BYTE stop_bits)
{
        assert(dcb->DCBlength == sizeof(*dcb));
        assert(dcb->BaudRate == baud && dcb->ByteSize == 8);
        assert(dcb->Parity == NOPARITY && dcb->StopBits == stop_bits);
        assert(dcb->fBinary && !dcb->fParity);
        /* No discarded NUL bytes, modem-dependent receive gating, replacement,
         * or a sticky error condition that prevents subsequent sensor reads. */
        assert(!dcb->fNull && !dcb->fDsrSensitivity && !dcb->fErrorChar);
        assert(!dcb->fAbortOnError);
        assert(!dcb->fOutxCtsFlow && !dcb->fOutxDsrFlow);
        assert(!dcb->fOutX && !dcb->fInX && dcb->fTXContinueOnXoff);
        assert(dcb->fDtrControl == DTR_CONTROL_DISABLE);
        assert(dcb->fRtsControl == RTS_CONTROL_DISABLE);
        assert(dcb->XonChar != dcb->XoffChar && dcb->XoffLim > 0);
        assert(dcb->EvtChar == 0); // Active streams enable EV_RXFLAG for NUL
        assert(dcb->fDummy2 == 0 && dcb->wReserved == 0 && dcb->wReserved1 == 0);
}

static void test_previous_application_cannot_change_configuration(void)
{
        DCB clean, contaminated;
        memset(&clean, 0, sizeof(clean));
        dyn200_normalize_binary_dcb(&clean, 115200, ONESTOPBIT);

        /* Include all receive filters/flow flags, invalid reserved bits and
         * equal XON/XOFF characters; GetCommState can return inherited flags. */
        memset(&contaminated, 0xff, sizeof(contaminated));
        contaminated.XonChar = contaminated.XoffChar = 0;
        dyn200_normalize_binary_dcb(&contaminated, 115200, ONESTOPBIT);
        assert_binary_no_flow(&contaminated, 115200, ONESTOPBIT);
        assert(memcmp(&clean, &contaminated, sizeof(clean)) == 0);

        /* Reopening with another wire format must update requested fields,
         * while producing the same filtering/control policy. */
        dyn200_normalize_binary_dcb(&contaminated, 9600, TWOSTOPBITS);
        assert_binary_no_flow(&contaminated, 9600, TWOSTOPBITS);
}

int main(void)
{
        test_previous_application_cannot_change_configuration();
        puts("dyn200 Windows serial configuration tests passed (no hardware)");
        return 0;
}
