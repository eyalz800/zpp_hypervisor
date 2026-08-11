#pragma once
// The sink list, which here is one sink. It exists as a file of its own
// because the real one does and resume.cpp includes it by name; the real
// one is a `sink_list` the pump walks, and nothing under test walks it.
#include "zpp/diag/sinks/esp_blocks.h"
