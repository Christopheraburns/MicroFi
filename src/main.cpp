// main.cpp -- MicroFi entry point when MicroFi owns the board (the XIAOs).
// Hosted builds (ESP-Brookesia on the AMOLED, #188) skip this file and call
// microfi_agent_start() from their own startup path.

#include "microfi/agent.h"

extern "C" void app_main(void) {
    microfi_agent_start();
}
