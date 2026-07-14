/** \file
 * \brief C++ SDK for accessing PDO freerun daemon shared memory (header-only)
 */

#ifndef PDO_FREERUN_SDK_H
#define PDO_FREERUN_SDK_H

#include "pdo_freerun_shm.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

class PdoFreerun
{
      public:
        PdoFreerun() : shm_(nullptr)
        {
#ifdef _WIN32
                hMap_ = nullptr;
#endif
        }

        ~PdoFreerun() { close(); }

        /** Init: open shared memory and validate magic/version. Returns true on success. */
        bool init()
        {
#ifdef _WIN32
                hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
                if (!hMap_)
                        return false;
                shm_ = (shm_header_t *)MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shm_header_t));
                if (!shm_) {
                        CloseHandle(hMap_);
                        hMap_ = nullptr;
                        return false;
                }
#endif
                if (!shm_ || shm_->magic != SHM_MAGIC || shm_->version != 1) {
                        close();
                        return false;
                }
                return true;
        }

        /** Close shared memory. */
        void close()
        {
#ifdef _WIN32
                if (shm_) {
                        UnmapViewOfFile(shm_);
                        shm_ = nullptr;
                }
                if (hMap_) {
                        CloseHandle(hMap_);
                        hMap_ = nullptr;
                }
#endif
        }

        /** Get raw shared memory pointer. */
        const shm_header_t *shm() const { return shm_; }

        /** Get number of slaves. */
        uint32_t scan_slave() const { return shm_ ? shm_->slave_count : 0; }

        /** Get slave output PDO data (read-only). Returns size in bytes, 0 if none.
         *  slave_idx is 0-based. */
        uint32_t get_output(uint32_t slave_idx, const uint8_t *&data) const
        {
                data = nullptr;
                if (!shm_ || slave_idx >= shm_->slave_count)
                        return 0;
                const shm_slave_pdo_t *s = &shm_->slaves[slave_idx];
                if (s->output_size == 0)
                        return 0;
                data = s->output_data;
                return s->output_size;
        }

        /** Get writable slave output PDO data. Returns size in bytes, 0 if none.
         *  slave_idx is 0-based. */
        uint32_t get_output(uint32_t slave_idx, uint8_t *&data)
        {
                data = nullptr;
                if (!shm_ || slave_idx >= shm_->slave_count)
                        return 0;
                shm_slave_pdo_t *s = &shm_->slaves[slave_idx];
                if (s->output_size == 0)
                        return 0;
                data = s->output_data;
                return s->output_size;
        }

        /** Get slave input PDO data. Returns size in bytes, 0 if none.
         *  slave_idx is 0-based. */
        uint32_t get_input(uint32_t slave_idx, const uint8_t *&data) const
        {
                data = nullptr;
                if (!shm_ || slave_idx >= shm_->slave_count)
                        return 0;
                const shm_slave_pdo_t *s = &shm_->slaves[slave_idx];
                if (s->input_size == 0)
                        return 0;
                data = s->input_data;
                return s->input_size;
        }

        /** Decode IEEE-754 binary16 into a 32-bit float. */
        static float f16_to_f32(float16_t h)
        {
                uint32_t sign = (uint32_t)(h >> 15) & 0x1u;
                uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
                uint32_t mant = (uint32_t)(h) & 0x3FFu;
                uint32_t f;

                if (exp == 0) {
                        if (mant == 0) {
                                f = sign << 31;
                        } else {
                                while ((mant & 0x400u) == 0) {
                                        mant <<= 1;
                                        exp   -= 1;
                                }
                                exp  += 1;
                                mant &= ~0x400u;
                                f     = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
                        }
                } else if (exp == 0x1F) {
                        f = (sign << 31) | (0xFFu << 23) | (mant << 13);
                } else {
                        f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
                }

                float out;
                std::memcpy(&out, &f, sizeof(out));
                return out;
        }

        /** Encode a 32-bit float into IEEE-754 binary16 (round-to-nearest-even). */
        static float16_t f32_to_f16(float v)
        {
                uint32_t x;
                std::memcpy(&x, &v, sizeof(x));

                uint32_t sign = (x >> 16) & 0x8000u;
                int32_t  exp  = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
                uint32_t mant = x & 0x7FFFFFu;

                if (((x >> 23) & 0xFFu) == 0xFFu) {
                        return (float16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
                }
                if (exp >= 0x1F) {
                        return (float16_t)(sign | 0x7C00u);
                }
                if (exp <= 0) {
                        if (exp < -10)
                                return (float16_t)sign;
                        mant = (mant | 0x800000u) >> (uint32_t)(1 - exp);
                        if (mant & 0x1000u)
                                mant += 0x2000u;
                        return (float16_t)(sign | (mant >> 13));
                }
                if (mant & 0x1000u) {
                        mant += 0x2000u;
                        if (mant & 0x800000u) {
                                mant  = 0;
                                exp  += 1;
                                if (exp >= 0x1F)
                                        return (float16_t)(sign | 0x7C00u);
                        }
                }
                return (float16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
        }

      private:
        shm_header_t *shm_;
#ifdef _WIN32
        HANDLE hMap_;
#endif
};

#endif /* PDO_FREERUN_SDK_H */
