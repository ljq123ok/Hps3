#include "Emu/Io/pad_config.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		if (repeat_duration_ms > 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
		}
		else
		{
			std::this_thread::yield();
		}
	}
}

[[noreturn]] void report_fatal_error(std::string_view, bool, bool)
{
	std::abort();
}

bool is_input_allowed()
{
	return true;
}
