#ifndef DYN200_SERIAL_WIN32_H
#define DYN200_SERIAL_WIN32_H

#include <string.h>
#include <windows.h>

/**
 * @brief CreateFile inherits the previous opener's DCB. Rebuild the binary, no-flow
 *        configuration so a terminal cannot change which sensor bytes we receive
 *        Keep the existing DTR/RTS-low policy: adapters may use RTS for RS485 direction
 */
static void dyn200_normalize_binary_dcb(DCB *dcb, DWORD baud, BYTE stop_bits)
{
        memset(dcb, 0, sizeof(*dcb));
        dcb->DCBlength = sizeof(*dcb);
        dcb->BaudRate = baud;
        dcb->ByteSize = 8;
        dcb->Parity = NOPARITY;
        dcb->StopBits = stop_bits;
        dcb->fBinary = TRUE;
        dcb->fParity = FALSE;
        dcb->fOutxCtsFlow = FALSE;
        dcb->fOutxDsrFlow = FALSE;
        dcb->fDtrControl = DTR_CONTROL_DISABLE;
        dcb->fDsrSensitivity = FALSE;
        dcb->fTXContinueOnXoff = TRUE;
        dcb->fOutX = FALSE;
        dcb->fInX = FALSE;
        dcb->fErrorChar = FALSE;
        dcb->fNull = FALSE;
        dcb->fRtsControl = RTS_CONTROL_DISABLE;
        dcb->fAbortOnError = FALSE;
        // SetCommState requires distinct XON/XOFF even when flow is disabled
        dcb->XonChar = 0x11;
        dcb->XoffChar = 0x13;
        dcb->XonLim = 128;
        dcb->XoffLim = 128;
}

#endif
