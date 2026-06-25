#ifndef SIGNALHANDLER_HPP
#define SIGNALHANDLER_HPP

#include <csignal>

extern volatile sig_atomic_t g_running;

void setupSignalHandlers();

#endif
