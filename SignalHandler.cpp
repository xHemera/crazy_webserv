#include "SignalHandler.hpp"

volatile sig_atomic_t g_running = 1;

static void signalHandler(int)
{
    g_running = 0;
}

void setupSignalHandlers()
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}
