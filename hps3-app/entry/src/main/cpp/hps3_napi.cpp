#include "napi/native_api.h"
#define KEY_ISO OHOS_XCOMPONENT_KEY_ISO
#include "ace/xcomponent/native_interface_xcomponent.h"
#undef KEY_ISO

// RPCS3's own logger writes to its log files (disabled in this build) and never
// reaches hilog, so native diagnostics that must be visible on-device go through
// OH_LOG_Print directly.
#include <hilog/log.h>

#define HPS3_LOG(fmt, ...) \
	OH_LOG_Print(LOG_APP, LOG_INFO, 0x3201, "HPS3_NATIVE", fmt, ##__VA_ARGS__)

#include "Crypto/key_vault.h"
#include "Crypto/unself.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Memory/vm.h"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/system_utils.hpp"
#include "Emu/system_progress.hpp"
#include "Emu/System.h"
#include "Emu/vfs_config.h"
#include "ohos_audio_backend.h"
#include "ohos_save_dialog.h"
#include "signal_abi_check.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNp.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Null/NullGSRender.h"
#if defined(HAVE_VULKAN)
#include "Emu/RSX/VK/VKGSRender.h"
#endif
#include "Input/pad_thread.h"
#include "Input/virtual_pad_handler.h"
#include "Loader/ISO.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Utilities/File.h"
#include "Utilities/JIT.h"
#include "util/logs.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include "util/video_source.h"
#include "Emu/RSX/VK/VulkanAPI.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <ucontext.h>

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;
extern std::atomic<u64> g_ohos_recovered_faults;

namespace
{
void install_segv_probe();

std::mutex g_emu_mutex;
bool g_initialized = false;
std::atomic<bool> g_install_active = false;
std::string g_runtime_root;

std::mutex g_game_image_mutex;
int g_game_image_fd = -1;
std::string g_game_image_path;

struct surface_state
{
	OHNativeWindow* window = nullptr;
	uint64_t width = 0;
	uint64_t height = 0;
	uint64_t epoch = 0;
};

std::mutex g_surface_mutex;
surface_state g_surface_state;

// T9 execution observability.
//
// Emu.GetStatus() alone cannot distinguish "the RSX thread was created" from
// "guest PPU/SPU code is actually retiring instructions", and both look like a
// live process to the OS. These counters give the ArkTS side independent,
// non-inferred evidence:
//   g_frame_count      - guest frames handed to the frontend by GSRender::flip()
//   g_active_renderer  - which GSRender backend init_gs_render() actually built
//   g_run_calls        - how many times the emulator left Ready for Starting
std::atomic<uint64_t> g_frame_count{0};
std::atomic<int32_t> g_active_renderer{0}; // 0=unknown, 1=NullGSRender, 2=VKGSRender
std::atomic<int32_t> g_run_calls{0};
std::atomic<int32_t> g_ready_calls{0};
std::atomic<int32_t> g_stop_calls{0};

// GSFrameBase implementation for HarmonyOS.
//
// Upstream implements handle() only in rpcs3qt/gs_frame.cpp, which this build
// excludes (no Qt). GSRender obtains its window from
// g_emu_callbacks.get_gs_frame(); while that returned nullptr, m_frame stayed
// null and every render action -- flip() above all -- was skipped, so the
// Vulkan RSX backend could never present. This adapter hands the RSX thread the
// OHNativeWindow owned by the ArkUI XComponent.
//
// Threading: handle()/client_width()/client_height()/display_epoch() are read by
// the RSX render thread while the XComponent callbacks that replace the window
// run on the UI thread, so every accessor locks g_surface_mutex.
class ohos_gs_frame final : public GSFrameBase
{
public:
	ohos_gs_frame() = default;

	void close() override {}
	void reset() override {}
	bool shown() override { return true; }
	void hide() override {}
	void show() override {}
	void toggle_fullscreen() override {}
	void delete_context(draw_context_t) override {}
	draw_context_t make_context() override { return nullptr; }
	void set_current(draw_context_t) override {}

	// Presentation belongs to the Vulkan swapchain built from handle().
	void flip(draw_context_t, bool = false) override
	{
		// Every guest frame is routed through here by GSRender::flip(), so this
		// counter proves the RSX thread actually reached its present stage
		// rather than merely having been constructed.
		g_frame_count.fetch_add(1);
	}

	int client_width() override
	{
		std::lock_guard lock(g_surface_mutex);
		return static_cast<int>(g_surface_state.width);
	}

	int client_height() override
	{
		std::lock_guard lock(g_surface_mutex);
		return static_cast<int>(g_surface_state.height);
	}

	f64 client_display_rate() override { return 60.0; }
	bool has_alpha() override { return false; }

	display_handle_t handle() const override
	{
		std::lock_guard lock(g_surface_mutex);
		return g_surface_state.window;
	}

	// Reported to the RSX backend so it can rebuild a swapchain that is still
	// bound to a replaced window even when the dimensions happen to match.
	u64 display_epoch() const override
	{
		std::lock_guard lock(g_surface_mutex);
		return g_surface_state.epoch;
	}

	bool can_consume_frame() const override { return false; }
	void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
	void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
	void update_title(double = 0.0) override {}
};

struct surface_probe_result
{
	bool ok = false;
	std::string code = "unknown";
	std::string message = "Vulkan surface 探针失败";
	std::string device_name;
	std::string api_version;
	int32_t queue_family = -1;
	int32_t format_count = 0;
};

struct swapchain_present_result
{
	bool ok = false;
	std::string code = "unknown";
	std::string message = "Vulkan swapchain 呈现测试失败";
	int32_t width = 0;
	int32_t height = 0;
	int32_t image_count = 0;
	int32_t format = 0;
};

struct swapchain_test_context
{
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkSemaphore image_available = VK_NULL_HANDLE;
	VkSemaphore render_finished = VK_NULL_HANDLE;
	VkFence render_fence = VK_NULL_HANDLE;
	std::vector<VkImageView> image_views;
	std::vector<VkFramebuffer> framebuffers;

	~swapchain_test_context()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
			if (render_fence != VK_NULL_HANDLE) vkDestroyFence(device, render_fence, nullptr);
			if (render_finished != VK_NULL_HANDLE) vkDestroySemaphore(device, render_finished, nullptr);
			if (image_available != VK_NULL_HANDLE) vkDestroySemaphore(device, image_available, nullptr);
			if (command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, command_pool, nullptr);
			for (VkFramebuffer framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
			if (render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, render_pass, nullptr);
			for (VkImageView image_view : image_views) vkDestroyImageView(device, image_view, nullptr);
			if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, surface, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

std::unique_ptr<swapchain_test_context> g_swapchain_test_context;

struct firmware_status
{
	bool active = false;
	int32_t completed = 0;
	int32_t total = 0;
	std::string phase = "idle";
	std::string message = "尚未安装固件";
	std::string version;
};

std::mutex g_firmware_status_mutex;
firmware_status g_firmware_status;

struct firmware_install_context
{
	napi_async_work work{};
	napi_deferred deferred{};
	std::string pup_path;
	bool ok = false;
	std::string code = "unknown";
	std::string message = "固件安装失败";
	std::string version;
	int32_t completed = 0;
	int32_t total = 0;
};

void update_firmware_status(bool active, std::string phase, std::string message,
	int32_t completed = 0, int32_t total = 0, std::string version = {})
{
	std::lock_guard lock(g_firmware_status_mutex);
	g_firmware_status.active = active;
	g_firmware_status.phase = std::move(phase);
	g_firmware_status.message = std::move(message);
	g_firmware_status.completed = completed;
	g_firmware_status.total = total;
	g_firmware_status.version = std::move(version);
}

napi_value make_string(napi_env env, const std::string& value)
{
	napi_value result{};
	napi_create_string_utf8(env, value.c_str(), value.size(), &result);
	return result;
}

void set_named(napi_env env, napi_value object, const char* name, napi_value value)
{
	napi_set_named_property(env, object, name, value);
}

napi_value make_int(napi_env env, int32_t value)
{
	napi_value result{};
	napi_create_int32(env, value, &result);
	return result;
}

napi_value make_int64(napi_env env, int64_t value)
{
	napi_value result{};
	napi_create_int64(env, value, &result);
	return result;
}

napi_value make_bool(napi_env env, bool value)
{
	napi_value result{};
	napi_get_boolean(env, value, &result);
	return result;
}

napi_value make_game_image_result(napi_env env, bool ok, const std::string& code,
	const std::string& message, int64_t size = 0, bool archive_valid = false,
	bool has_param_sfo = false, bool has_eboot = false, const std::string& title = {},
	const std::string& title_id = {}, const std::string& category = {},
	const std::string& path = {})
{
	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "ok", make_bool(env, ok));
	set_named(env, result, "code", make_string(env, code));
	set_named(env, result, "message", make_string(env, message));
	set_named(env, result, "size", make_int64(env, size));
	set_named(env, result, "archiveValid", make_bool(env, archive_valid));
	set_named(env, result, "hasParamSfo", make_bool(env, has_param_sfo));
	set_named(env, result, "hasEboot", make_bool(env, has_eboot));
	set_named(env, result, "title", make_string(env, title));
	set_named(env, result, "titleId", make_string(env, title_id));
	set_named(env, result, "category", make_string(env, category));
	set_named(env, result, "path", make_string(env, path));
	return result;
}

napi_value is_core_linked(napi_env env, napi_callback_info)
{
	return make_bool(env, true);
}

void configure_headless_callbacks()
{
	virtual_pad_handler::set_on_connect_cb([](const std::shared_ptr<Pad>& pad)
	{
		if (!pad || pad->m_player_id != 0) return false;
		pad->Init(CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES,
			CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE |
			CELL_PAD_CAPABILITY_HP_ANALOG_STICK,
			CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD, 0, 0x054c, 0x0268, 50);
		const u32 bits[] = {CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT,
			CELL_PAD_CTRL_RIGHT, CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_SQUARE,
			CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_TRIANGLE, CELL_PAD_CTRL_L1,
			CELL_PAD_CTRL_L2, CELL_PAD_CTRL_L3, CELL_PAD_CTRL_R1, CELL_PAD_CTRL_R2,
			CELL_PAD_CTRL_R3, CELL_PAD_CTRL_START, CELL_PAD_CTRL_SELECT};
		for (u32 i = 0; i < std::size(bits); ++i)
		{
			const bool digital1 = i < 4 || i == 10 || i >= 13;
			pad->m_buttons.emplace_back(digital1 ? CELL_PAD_BTN_OFFSET_DIGITAL1 : CELL_PAD_BTN_OFFSET_DIGITAL2,
				std::vector<std::set<u32>>{}, bits[i]);
		}
		pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
		pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
		pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
		pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});
		return true;
	});
	g_emu_callbacks = {};
	g_emu_callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
	{
		if (func) func();
		if (wake_up)
		{
			*wake_up = true;
			wake_up->notify_one();
		}
	};
	g_emu_callbacks.try_to_quit = [](bool, std::function<void()> on_exit)
	{
		if (on_exit) on_exit();
		return true;
	};
	g_emu_callbacks.init_kb_handler = []()
	{
		ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
	};
	g_emu_callbacks.init_mouse_handler = []()
	{
		ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
	};
	g_emu_callbacks.init_pad_handler = [](std::string_view title_id)
	{
		ensure(g_fxo->init<named_thread<pad_thread>>(static_cast<void*>(nullptr),
			static_cast<void*>(nullptr), title_id));
	};
	g_emu_callbacks.init_gs_render = [](utils::serial* ar)
	{
		// Mirrors the desktop switch in rpcs3qt/gui_application.cpp. Vulkan needs
		// a live OHNativeWindow: without one create_swapchain() fails and the
		// renderer is left with a null device, so fall back to the Null backend
		// whenever the XComponent has not handed us a surface yet.
		bool have_surface = false;
		uint64_t sw = 0, sh = 0, sep = 0;
		{
			std::lock_guard lock(g_surface_mutex);
			have_surface = g_surface_state.window != nullptr &&
				g_surface_state.width > 0 && g_surface_state.height > 0;
			sw = g_surface_state.width;
			sh = g_surface_state.height;
			sep = g_surface_state.epoch;
		}

#if defined(HAVE_VULKAN)
		const bool want_vulkan = g_cfg.video.renderer.get() == video_renderer::vulkan;
		HPS3_LOG("HPS3_GS_PICK want_vulkan=%d have_surface=%d size=%llux%llu epoch=%llu",
			want_vulkan ? 1 : 0, have_surface ? 1 : 0,
			static_cast<unsigned long long>(sw), static_cast<unsigned long long>(sh),
			static_cast<unsigned long long>(sep));
#else
		const bool want_vulkan = false;
		HPS3_LOG("HPS3_GS_PICK build has no Vulkan support");
#endif

		if (have_surface && want_vulkan)
		{
#if defined(HAVE_VULKAN)
			// The T6 diagnostic test pattern keeps a live VkSwapchainKHR on this
			// same OHNativeWindow. A native window can back only one swapchain, so
			// leaving it alive makes vkCreateSwapchainKHR below fail with
			// VK_ERROR_NATIVE_WINDOW_IN_USE_KHR (-1000000000) and leaves
			// m_vk_swapchain null, which then faults inside the driver.
			g_swapchain_test_context.reset();
			// Log the exact window pointer that VKGSRender will read back through
			// handle(). If this is 0 the surface creation below dereferences null.
			HPS3_LOG("HPS3_GS_PICK -> VKGSRender window=%p size=%llux%llu",
				static_cast<void*>(g_surface_state.window),
				static_cast<unsigned long long>(sw), static_cast<unsigned long long>(sh));
			g_active_renderer.store(2);
			g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
			HPS3_LOG("HPS3_GS_PICK VKGSRender ctor returned");
			return;
#endif
		}

		HPS3_LOG("HPS3_GS_PICK -> NullGSRender");
		g_active_renderer.store(1);
		g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
	};
	g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
	{
		return std::make_shared<ohos_audio_backend>();
	};
	g_emu_callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator>
	{
		return std::make_shared<null_enumerator>();
	};
	g_emu_callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base>
	{
		return std::make_shared<null_camera_handler>();
	};
	g_emu_callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base>
	{
		return std::make_shared<null_music_handler>();
	};
	g_emu_callbacks.close_gs_frame = []() {};
	// Returns the XComponent-backed window so RSX can open a Vulkan swapchain.
	// This runs on the RSX thread during Emulator::Load, i.e. while Init() is
	// still in flight; see the renderer selection in initialize() for why the
	// Vulkan backend is only armed once a real surface exists.
	g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
	{
		return std::make_unique<ohos_gs_frame>();
	};
	g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
	g_emu_callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
	g_emu_callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return std::make_unique<ohos_save_dialog>(); };
	g_emu_callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
	g_emu_callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
	g_emu_callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };
	g_emu_callbacks.update_emu_settings = []() {};
	g_emu_callbacks.save_emu_settings = []() {};
	g_emu_callbacks.on_run = [](bool start_playtime)
	{
		g_run_calls.fetch_add(1);
		HPS3_LOG("HPS3_ON_RUN start_playtime=%d state=%d", start_playtime ? 1 : 0,
			static_cast<int32_t>(Emu.GetStatus()));
	};
	g_emu_callbacks.on_pause = []() {};
	g_emu_callbacks.on_resume = []() {};
	g_emu_callbacks.on_stop = []()
	{
		g_stop_calls.fetch_add(1);
		HPS3_LOG("HPS3_ON_STOP state=%d frames=%llu", static_cast<int32_t>(Emu.GetStatus()),
			static_cast<unsigned long long>(g_frame_count.load()));
	};
	g_emu_callbacks.on_ready = []()
	{
		g_ready_calls.fetch_add(1);
		HPS3_LOG("HPS3_ON_READY state=%d", static_cast<int32_t>(Emu.GetStatus()));
	};
	g_emu_callbacks.on_missing_fw = []() {};
	g_emu_callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
	g_emu_callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>,
		stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
	g_emu_callbacks.enable_disc_eject = [](bool) {};
	g_emu_callbacks.enable_disc_insert = [](bool) {};
	g_emu_callbacks.handle_taskbar_progress = [](s32, s32) {};
	g_emu_callbacks.get_localized_string = hps3_localized_string;
	g_emu_callbacks.get_localized_u32string = [](localized_string_id id, const char* args) -> std::u32string
	{
		static_assert(sizeof(wchar_t) == sizeof(char32_t));
		const auto text = utf8_to_wchar(hps3_localized_string(id, args));
		return {text.begin(), text.end()};
	};
	g_emu_callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
	g_emu_callbacks.get_photo_path = [](std::string_view) -> std::string { return {}; };
	g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
	g_emu_callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
	g_emu_callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
	g_emu_callbacks.get_font_dirs = []() -> std::vector<std::string> { return {}; };
	g_emu_callbacks.on_install_pkgs = [](const std::vector<std::string>& pkgs, bool from_optical_drive)
	{
		for (const std::string& pkg : pkgs)
		{
			if (!rpcs3::utils::install_pkg(pkg, from_optical_drive)) return false;
		}
		return true;
	};
	g_emu_callbacks.add_breakpoint = [](u32) {};
	g_emu_callbacks.display_sleep_control_supported = []() { return false; };
	g_emu_callbacks.enable_display_sleep = [](bool) {};
	g_emu_callbacks.check_microphone_permissions = []() {};
	g_emu_callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return {}; };
	g_emu_callbacks.enable_gamemode = [](bool) {};
	g_emu_callbacks.get_database_config = [](const std::string&) -> std::string { return {}; };
}

// RPCS3's logs:: system drops every message unless a listener is registered, and
// only the Qt and Android frontends ever registered one. That is why RSX/PPU
// diagnostics were invisible here: the messages were never written anywhere.
//
// logs::make_file_listener() is not usable on this target -- its file_writer also
// opens "<name>.gz" and calls deflateInit2(), and this build does not link zlib,
// so those symbols stay undefined and the process aborts the moment the listener
// is constructed (SIGABRT right after loadContent). Forward to hilog instead,
// which needs no extra dependency and shows up in the normal device log stream.
class hilog_listener final : public logs::listener
{
public:
	void log(u64, const logs::message& msg, std::string_view prefix, std::string_view text) override
	{
		const auto severity = static_cast<logs::level>(msg);
		if (severity != logs::level::fatal && severity != logs::level::error && severity != logs::level::warning)
			return;
		char level = 'I';
		switch (static_cast<logs::level>(msg))
		{
		case logs::level::fatal:
		case logs::level::error:   level = 'E'; break;
		case logs::level::warning: level = 'W'; break;
		case logs::level::success: level = 'I'; break;
		case logs::level::notice:
		case logs::level::todo:    level = 'I'; break;
		default:                   level = 'D'; break;
		}

		// Keep the prefix (channel + PPU/SPU thread name): it is what makes the
		// log readable, e.g. "PPU[0x1000000] main_thread".
		const std::string line = std::string(prefix) + std::string(text);
		OH_LOG_Print(LOG_APP,
			level == 'E' ? LOG_ERROR : (level == 'W' ? LOG_WARN : LOG_INFO),
			0x3201, "HPS3_CORE", "%{public}s", line.c_str());
	}
};

std::unique_ptr<hilog_listener> g_log_listener;

void install_log_listener()
{
	if (g_log_listener || !Emu.IsReady())
	{
		return;
	}

	// Register after the core has finished booting, without the file listener's
	// extra initialization or logging every successful guest operation.
	g_log_listener = std::make_unique<hilog_listener>();
	logs::listener::add(g_log_listener.get());
	logs::set_init({});
	HPS3_LOG("HPS3_LOG_LISTENER enabled");
}

napi_value get_state(napi_env env, napi_callback_info)
{
	napi_value result{};
	napi_create_int32(env, static_cast<int32_t>(Emu.GetStatus()), &result);
	return result;
}

void update_surface(OH_NativeXComponent* component, void* window)
{
	uint64_t width = 0;
	uint64_t height = 0;
	if (component && window)
	{
		OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);
	}

	std::lock_guard lock(g_surface_mutex);
	g_swapchain_test_context.reset();
	g_surface_state.window = static_cast<OHNativeWindow*>(window);
	g_surface_state.width = width;
	g_surface_state.height = height;
	g_surface_state.epoch++;
}

void on_surface_created(OH_NativeXComponent* component, void* window)
{
	update_surface(component, window);
}

void on_surface_changed(OH_NativeXComponent* component, void* window)
{
	update_surface(component, window);
}

void on_surface_destroyed(OH_NativeXComponent*, void* window)
{
	std::lock_guard lock(g_surface_mutex);
	if (g_surface_state.window == window)
	{
		g_swapchain_test_context.reset();
		g_surface_state.window = nullptr;
		g_surface_state.width = 0;
		g_surface_state.height = 0;
		g_surface_state.epoch++;
	}
}

void on_surface_touch(OH_NativeXComponent*, void*)
{
}

OH_NativeXComponent_Callback g_xcomponent_callback = {
	.OnSurfaceCreated = on_surface_created,
	.OnSurfaceChanged = on_surface_changed,
	.OnSurfaceDestroyed = on_surface_destroyed,
	.DispatchTouchEvent = on_surface_touch,
};

void register_xcomponent(napi_env env, napi_value exports)
{
	napi_value wrapped{};
	if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &wrapped) != napi_ok)
	{
		return;
	}

	OH_NativeXComponent* component = nullptr;
	if (napi_unwrap(env, wrapped, reinterpret_cast<void**>(&component)) != napi_ok || !component)
	{
		return;
	}
	OH_NativeXComponent_RegisterCallback(component, &g_xcomponent_callback);
}

napi_value get_surface_status(napi_env env, napi_callback_info)
{
	surface_state state;
	{
		std::lock_guard lock(g_surface_mutex);
		state = g_surface_state;
	}

	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "available", make_bool(env, state.window != nullptr));
	set_named(env, result, "width", make_int64(env, static_cast<int64_t>(state.width)));
	set_named(env, result, "height", make_int64(env, static_cast<int64_t>(state.height)));
	set_named(env, result, "epoch", make_int64(env, static_cast<int64_t>(state.epoch)));
	return result;
}

surface_probe_result run_surface_probe()
{
	surface_probe_result result;
	OHNativeWindow* window = nullptr;
	{
		std::lock_guard lock(g_surface_mutex);
		window = g_surface_state.window;
	}
	if (!window)
	{
		result.code = "surface_unavailable";
		result.message = "XComponent 尚未提供 NativeWindow";
		return result;
	}

	uint32_t loader_version = VK_API_VERSION_1_0;
	if (vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS)
	{
		loader_version = VK_API_VERSION_1_0;
	}
	const uint32_t requested_version = std::min(loader_version, static_cast<uint32_t>(VK_API_VERSION_1_2));
	result.api_version = std::to_string(VK_VERSION_MAJOR(requested_version)) + "." +
		std::to_string(VK_VERSION_MINOR(requested_version)) + "." +
		std::to_string(VK_VERSION_PATCH(requested_version));

	VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "hps3-surface-probe";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "hps3";
	app_info.engineVersion = 1;
	app_info.apiVersion = requested_version;
	const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
	VkInstanceCreateInfo instance_info{};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app_info;
	instance_info.enabledExtensionCount = 2;
	instance_info.ppEnabledExtensionNames = extensions;

	VkInstance instance = VK_NULL_HANDLE;
	VkResult vk_result = vkCreateInstance(&instance_info, nullptr, &instance);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "instance_failed";
		result.message = "vkCreateInstance 失败：" + std::to_string(vk_result);
		return result;
	}

	VkSurfaceCreateInfoOHOS surface_info{};
	surface_info.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
	surface_info.window = window;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	vk_result = vkCreateSurfaceOHOS(instance, &surface_info, nullptr, &surface);
	if (vk_result != VK_SUCCESS)
	{
		vkDestroyInstance(instance, nullptr);
		result.code = "surface_create_failed";
		result.message = "vkCreateSurfaceOHOS 失败：" + std::to_string(vk_result);
		return result;
	}

	uint32_t device_count = 0;
	vk_result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
	if (vk_result != VK_SUCCESS || device_count == 0)
	{
		vkDestroySurfaceKHR(instance, surface, nullptr);
		vkDestroyInstance(instance, nullptr);
		result.code = "no_physical_device";
		result.message = "没有可用的 Vulkan 物理设备";
		return result;
	}

	std::vector<VkPhysicalDevice> devices(device_count);
	vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
	for (VkPhysicalDevice device : devices)
	{
		uint32_t queue_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(queue_count);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_count, queues.data());
		for (uint32_t index = 0; index < queue_count; index++)
		{
			VkBool32 present_supported = VK_FALSE;
			if (vkGetPhysicalDeviceSurfaceSupportKHR(device, index, surface, &present_supported) == VK_SUCCESS &&
				present_supported == VK_TRUE)
			{
				VkPhysicalDeviceProperties properties{};
				vkGetPhysicalDeviceProperties(device, &properties);
				uint32_t format_count = 0;
				vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
				result.ok = true;
				result.code = "ok";
				result.message = "VK_OHOS_surface 创建成功且 GPU 支持 present";
				result.device_name = properties.deviceName;
				result.queue_family = static_cast<int32_t>(index);
				result.format_count = static_cast<int32_t>(format_count);
				break;
			}
		}
		if (result.ok)
		{
			break;
		}
	}

	if (!result.ok)
	{
		result.code = "present_unsupported";
		result.message = "Vulkan 设备不支持向当前 XComponent surface 呈现";
	}
	vkDestroySurfaceKHR(instance, surface, nullptr);
	vkDestroyInstance(instance, nullptr);
	return result;
}

napi_value probe_vulkan_surface(napi_env env, napi_callback_info)
{
	const surface_probe_result probe = run_surface_probe();
	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "ok", make_bool(env, probe.ok));
	set_named(env, result, "code", make_string(env, probe.code));
	set_named(env, result, "message", make_string(env, probe.message));
	set_named(env, result, "deviceName", make_string(env, probe.device_name));
	set_named(env, result, "apiVersion", make_string(env, probe.api_version));
	set_named(env, result, "queueFamily", make_int(env, probe.queue_family));
	set_named(env, result, "formatCount", make_int(env, probe.format_count));
	return result;
}

swapchain_present_result run_swapchain_present_test()
{
	swapchain_present_result result;
	std::unique_lock surface_lock(g_surface_mutex);
	OHNativeWindow* window = g_surface_state.window;
	const uint32_t requested_width = static_cast<uint32_t>(g_surface_state.width);
	const uint32_t requested_height = static_cast<uint32_t>(g_surface_state.height);
	if (!window || requested_width == 0 || requested_height == 0)
	{
		result.code = "surface_unavailable";
		result.message = "XComponent 尚未提供有效 NativeWindow";
		return result;
	}

	g_swapchain_test_context.reset();
	auto owned_context = std::make_unique<swapchain_test_context>();
	swapchain_test_context& context = *owned_context;
	VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "hps3-swapchain-test";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "hps3";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_API_VERSION_1_0;
	const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
	VkInstanceCreateInfo instance_info{};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app_info;
	instance_info.enabledExtensionCount = 2;
	instance_info.ppEnabledExtensionNames = instance_extensions;
	VkResult vk_result = vkCreateInstance(&instance_info, nullptr, &context.instance);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "instance_failed";
		result.message = "vkCreateInstance 失败：" + std::to_string(vk_result);
		return result;
	}

	VkSurfaceCreateInfoOHOS surface_info{};
	surface_info.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
	surface_info.window = window;
	vk_result = vkCreateSurfaceOHOS(context.instance, &surface_info, nullptr, &context.surface);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "surface_create_failed";
		result.message = "vkCreateSurfaceOHOS 失败：" + std::to_string(vk_result);
		return result;
	}

	uint32_t physical_device_count = 0;
	vk_result = vkEnumeratePhysicalDevices(context.instance, &physical_device_count, nullptr);
	if (vk_result != VK_SUCCESS || physical_device_count == 0)
	{
		result.code = "no_physical_device";
		result.message = "没有可用的 Vulkan 物理设备";
		return result;
	}
	std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
	vkEnumeratePhysicalDevices(context.instance, &physical_device_count, physical_devices.data());

	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	uint32_t queue_family = std::numeric_limits<uint32_t>::max();
	for (VkPhysicalDevice candidate : physical_devices)
	{
		uint32_t queue_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(queue_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
		for (uint32_t index = 0; index < queue_count; index++)
		{
			VkBool32 present_supported = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, context.surface, &present_supported);
			if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_supported == VK_TRUE)
			{
				physical_device = candidate;
				queue_family = index;
				break;
			}
		}
		if (physical_device != VK_NULL_HANDLE) break;
	}
	if (physical_device == VK_NULL_HANDLE)
	{
		result.code = "no_graphics_present_queue";
		result.message = "没有同时支持 graphics 与 present 的队列";
		return result;
	}

	const float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &queue_priority;
	const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo device_info{};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 1;
	device_info.ppEnabledExtensionNames = device_extensions;
	vk_result = vkCreateDevice(physical_device, &device_info, nullptr, &context.device);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "device_failed";
		result.message = "vkCreateDevice/VK_KHR_swapchain 失败：" + std::to_string(vk_result);
		return result;
	}
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(context.device, queue_family, 0, &queue);

	VkSurfaceCapabilitiesKHR capabilities{};
	vk_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, context.surface, &capabilities);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "capabilities_failed";
		result.message = "读取 surface capabilities 失败：" + std::to_string(vk_result);
		return result;
	}
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
	{
		result.code = "color_attachment_unsupported";
		result.message = "surface 不支持 color attachment swapchain image";
		return result;
	}

	uint32_t format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, context.surface, &format_count, nullptr);
	if (format_count == 0)
	{
		result.code = "no_surface_format";
		result.message = "surface 没有可用像素格式";
		return result;
	}
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, context.surface, &format_count, formats.data());
	VkSurfaceFormatKHR selected_format = formats[0];
	if (selected_format.format == VK_FORMAT_UNDEFINED)
	{
		selected_format.format = VK_FORMAT_B8G8R8A8_UNORM;
	}
	else
	{
		for (const VkSurfaceFormatKHR& candidate : formats)
		{
			if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
			{
				selected_format = candidate;
				break;
			}
		}
	}

	VkExtent2D extent = capabilities.currentExtent;
	if (extent.width == std::numeric_limits<uint32_t>::max())
	{
		extent.width = std::clamp(requested_width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
		extent.height = std::clamp(requested_height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
	}
	uint32_t image_count = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0) image_count = std::min(image_count, capabilities.maxImageCount);
	VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((capabilities.supportedCompositeAlpha & composite_alpha) == 0)
	{
		if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
			composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
			composite_alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
		else
			composite_alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	}

	VkSwapchainCreateInfoKHR swapchain_info{};
	swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_info.surface = context.surface;
	swapchain_info.minImageCount = image_count;
	swapchain_info.imageFormat = selected_format.format;
	swapchain_info.imageColorSpace = selected_format.colorSpace;
	swapchain_info.imageExtent = extent;
	swapchain_info.imageArrayLayers = 1;
	swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_info.preTransform = capabilities.currentTransform;
	swapchain_info.compositeAlpha = composite_alpha;
	swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchain_info.clipped = VK_TRUE;
	vk_result = vkCreateSwapchainKHR(context.device, &swapchain_info, nullptr, &context.swapchain);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "swapchain_failed";
		result.message = "vkCreateSwapchainKHR 失败：" + std::to_string(vk_result);
		return result;
	}

	vkGetSwapchainImagesKHR(context.device, context.swapchain, &image_count, nullptr);
	std::vector<VkImage> images(image_count);
	vkGetSwapchainImagesKHR(context.device, context.swapchain, &image_count, images.data());
	context.image_views.reserve(image_count);
	for (VkImage image : images)
	{
		VkImageViewCreateInfo view_info{};
		view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image = image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = selected_format.format;
		view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
		view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.levelCount = 1;
		view_info.subresourceRange.layerCount = 1;
		VkImageView image_view = VK_NULL_HANDLE;
		vk_result = vkCreateImageView(context.device, &view_info, nullptr, &image_view);
		if (vk_result != VK_SUCCESS)
		{
			result.code = "image_view_failed";
			result.message = "vkCreateImageView 失败：" + std::to_string(vk_result);
			return result;
		}
		context.image_views.push_back(image_view);
	}

	VkAttachmentDescription color_attachment{};
	color_attachment.format = selected_format.format;
	color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference color_reference{};
	color_reference.attachment = 0;
	color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_reference;
	VkSubpassDependency dependency{};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	VkRenderPassCreateInfo render_pass_info{};
	render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render_pass_info.attachmentCount = 1;
	render_pass_info.pAttachments = &color_attachment;
	render_pass_info.subpassCount = 1;
	render_pass_info.pSubpasses = &subpass;
	render_pass_info.dependencyCount = 1;
	render_pass_info.pDependencies = &dependency;
	vk_result = vkCreateRenderPass(context.device, &render_pass_info, nullptr, &context.render_pass);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "render_pass_failed";
		result.message = "vkCreateRenderPass 失败：" + std::to_string(vk_result);
		return result;
	}

	context.framebuffers.reserve(image_count);
	for (VkImageView image_view : context.image_views)
	{
		VkFramebufferCreateInfo framebuffer_info{};
		framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass = context.render_pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments = &image_view;
		framebuffer_info.width = extent.width;
		framebuffer_info.height = extent.height;
		framebuffer_info.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		vk_result = vkCreateFramebuffer(context.device, &framebuffer_info, nullptr, &framebuffer);
		if (vk_result != VK_SUCCESS)
		{
			result.code = "framebuffer_failed";
			result.message = "vkCreateFramebuffer 失败：" + std::to_string(vk_result);
			return result;
		}
		context.framebuffers.push_back(framebuffer);
	}

	VkCommandPoolCreateInfo command_pool_info{};
	command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	command_pool_info.queueFamilyIndex = queue_family;
	vk_result = vkCreateCommandPool(context.device, &command_pool_info, nullptr, &context.command_pool);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "command_pool_failed";
		result.message = "vkCreateCommandPool 失败：" + std::to_string(vk_result);
		return result;
	}
	VkCommandBufferAllocateInfo command_buffer_info{};
	command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_buffer_info.commandPool = context.command_pool;
	command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_buffer_info.commandBufferCount = 1;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	vk_result = vkAllocateCommandBuffers(context.device, &command_buffer_info, &command_buffer);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "command_buffer_failed";
		result.message = "vkAllocateCommandBuffers 失败：" + std::to_string(vk_result);
		return result;
	}

	VkSemaphoreCreateInfo semaphore_info{};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateSemaphore(context.device, &semaphore_info, nullptr, &context.image_available) != VK_SUCCESS ||
		vkCreateSemaphore(context.device, &semaphore_info, nullptr, &context.render_finished) != VK_SUCCESS ||
		vkCreateFence(context.device, &fence_info, nullptr, &context.render_fence) != VK_SUCCESS)
	{
		result.code = "sync_create_failed";
		result.message = "创建 Vulkan 同步对象失败";
		return result;
	}

	uint32_t image_index = 0;
	vk_result = vkAcquireNextImageKHR(context.device, context.swapchain,
		std::numeric_limits<uint64_t>::max(), context.image_available, VK_NULL_HANDLE, &image_index);
	if (vk_result != VK_SUCCESS && vk_result != VK_SUBOPTIMAL_KHR)
	{
		result.code = "acquire_failed";
		result.message = "vkAcquireNextImageKHR 失败：" + std::to_string(vk_result);
		return result;
	}

	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command_buffer, &begin_info);
	VkClearValue clear_value{};
	clear_value.color.float32[0] = 0.03f;
	clear_value.color.float32[1] = 0.50f;
	clear_value.color.float32[2] = 0.72f;
	clear_value.color.float32[3] = 1.0f;
	VkRenderPassBeginInfo pass_info{};
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass_info.renderPass = context.render_pass;
	pass_info.framebuffer = context.framebuffers[image_index];
	pass_info.renderArea.extent = extent;
	pass_info.clearValueCount = 1;
	pass_info.pClearValues = &clear_value;
	vkCmdBeginRenderPass(command_buffer, &pass_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(command_buffer);
	vk_result = vkEndCommandBuffer(command_buffer);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "command_record_failed";
		result.message = "记录 Vulkan 清屏命令失败：" + std::to_string(vk_result);
		return result;
	}

	const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit_info{};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.waitSemaphoreCount = 1;
	submit_info.pWaitSemaphores = &context.image_available;
	submit_info.pWaitDstStageMask = &wait_stage;
	submit_info.commandBufferCount = 1;
	submit_info.pCommandBuffers = &command_buffer;
	submit_info.signalSemaphoreCount = 1;
	submit_info.pSignalSemaphores = &context.render_finished;
	vk_result = vkQueueSubmit(queue, 1, &submit_info, context.render_fence);
	if (vk_result != VK_SUCCESS)
	{
		result.code = "submit_failed";
		result.message = "vkQueueSubmit 失败：" + std::to_string(vk_result);
		return result;
	}
	VkPresentInfoKHR present_info{};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &context.render_finished;
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &context.swapchain;
	present_info.pImageIndices = &image_index;
	vk_result = vkQueuePresentKHR(queue, &present_info);
	if (vk_result != VK_SUCCESS && vk_result != VK_SUBOPTIMAL_KHR)
	{
		result.code = "present_failed";
		result.message = "vkQueuePresentKHR 失败：" + std::to_string(vk_result);
		return result;
	}
	vkWaitForFences(context.device, 1, &context.render_fence, VK_TRUE,
		std::numeric_limits<uint64_t>::max());
	vkQueueWaitIdle(queue);

	result.ok = true;
	result.code = "ok";
	result.message = "swapchain 已创建并保持呈现 Vulkan 测试色块（非游戏画面）";
	result.width = static_cast<int32_t>(extent.width);
	result.height = static_cast<int32_t>(extent.height);
	result.image_count = static_cast<int32_t>(image_count);
	result.format = static_cast<int32_t>(selected_format.format);
	g_swapchain_test_context = std::move(owned_context);
	return result;
}

napi_value present_vulkan_test_pattern(napi_env env, napi_callback_info)
{
	const swapchain_present_result present = run_swapchain_present_test();
	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "ok", make_bool(env, present.ok));
	set_named(env, result, "code", make_string(env, present.code));
	set_named(env, result, "message", make_string(env, present.message));
	set_named(env, result, "width", make_int(env, present.width));
	set_named(env, result, "height", make_int(env, present.height));
	set_named(env, result, "imageCount", make_int(env, present.image_count));
	set_named(env, result, "format", make_int(env, present.format));
	return result;
}

napi_value get_installed_firmware_version(napi_env env, napi_callback_info)
{
	std::lock_guard lock(g_emu_mutex);
	return make_string(env, g_initialized ? utils::get_firmware_version() : std::string{});
}

napi_value get_firmware_install_status(napi_env env, napi_callback_info)
{
	firmware_status status;
	{
		std::lock_guard lock(g_firmware_status_mutex);
		status = g_firmware_status;
	}

	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "active", make_bool(env, status.active));
	set_named(env, result, "completed", make_int(env, status.completed));
	set_named(env, result, "total", make_int(env, status.total));
	set_named(env, result, "phase", make_string(env, status.phase));
	set_named(env, result, "message", make_string(env, status.message));
	set_named(env, result, "version", make_string(env, status.version));
	return result;
}

void fail_install(firmware_install_context& context, std::string code, std::string message)
{
	context.ok = false;
	context.code = std::move(code);
	context.message = std::move(message);
	update_firmware_status(false, "failed", context.message, context.completed, context.total, context.version);
}

bool restore_live_mount(const std::string& live_path)
{
	return vfs::mount("/dev_flash", live_path + '/');
}

void run_firmware_install(firmware_install_context& context)
{
	std::lock_guard emu_lock(g_emu_mutex);
	if (!g_initialized)
	{
		fail_install(context, "not_initialized", "模拟器核心尚未初始化");
		return;
	}
	if (!Emu.IsStopped())
	{
		fail_install(context, "emulator_busy", "模拟器正在运行，不能安装固件");
		return;
	}

	update_firmware_status(true, "validating", "正在校验 PUP 文件");
	fs::file pup_file(context.pup_path);
	if (!pup_file)
	{
		fail_install(context, "open_failed", "无法打开所选 PUP 文件");
		return;
	}

	pup_object pup(std::move(pup_file));
	switch (static_cast<pup_error>(pup))
	{
	case pup_error::ok:
		break;
	case pup_error::header_read:
		fail_install(context, "empty_pup", "PUP 文件为空");
		return;
	case pup_error::header_magic:
		fail_install(context, "not_pup", "所选文件不是有效的 PUP 固件");
		return;
	case pup_error::expected_size:
		fail_install(context, "incomplete_pup", "PUP 文件不完整");
		return;
	case pup_error::hash_mismatch:
		fail_install(context, "hash_mismatch", "PUP 内容校验失败");
		return;
	default:
		fail_install(context, "corrupt_pup", "PUP 文件已损坏或格式无效");
		return;
	}

	if (fs::file version_file = pup.get_file(0x100))
	{
		context.version = version_file.to_string();
		if (const auto newline = context.version.find('\n'); newline != std::string::npos)
		{
			context.version.erase(newline);
		}
	}
	if (context.version.empty())
	{
		fail_install(context, "missing_version", "PUP 中没有固件版本信息");
		return;
	}

	fs::file update_files_file = pup.get_file(0x300);
	if (!update_files_file || update_files_file.size() == 0)
	{
		fail_install(context, "missing_packages", "PUP 中没有固件安装包数据库");
		return;
	}

	tar_object update_files(update_files_file);
	auto filenames = update_files.get_filenames();
	filenames.erase(std::remove_if(filenames.begin(), filenames.end(), [](const std::string& name)
	{
		return name.find("dev_flash_") == std::string::npos;
	}), filenames.end());
	if (filenames.empty())
	{
		fail_install(context, "missing_dev_flash", "PUP 中没有 dev_flash 安装包");
		return;
	}

	context.total = static_cast<int32_t>(filenames.size());
	update_firmware_status(true, "preparing", "正在准备事务安装目录", 0, context.total, context.version);

	std::string live_path = g_cfg_vfs.get_dev_flash();
	while (live_path.size() > 1 && live_path.back() == '/')
	{
		live_path.pop_back();
	}
	if (live_path.empty() || live_path == "/" || live_path.find("dev_flash") == std::string::npos)
	{
		fail_install(context, "unsafe_target", "固件目标目录校验失败");
		return;
	}

	const std::string staging_path = live_path + ".installing";
	const std::string backup_path = live_path + ".backup";
	std::error_code error;

	// Recover an interrupted commit before starting a new one. A backup is only
	// discarded when a live tree is already present.
	if (std::filesystem::exists(backup_path, error) && !std::filesystem::exists(live_path, error))
	{
		std::filesystem::rename(backup_path, live_path, error);
		if (error)
		{
			fail_install(context, "recovery_failed", "上次固件安装的备份恢复失败");
			return;
		}
	}
	if (std::filesystem::exists(backup_path, error) && std::filesystem::exists(live_path, error))
	{
		std::filesystem::remove_all(backup_path, error);
		if (error)
		{
			fail_install(context, "backup_cleanup_failed", "旧固件备份清理失败");
			return;
		}
	}
	std::filesystem::remove_all(staging_path, error);
	if (error)
	{
		fail_install(context, "staging_cleanup_failed", "临时安装目录清理失败");
		return;
	}
	std::filesystem::create_directories(staging_path, error);
	if (error)
	{
		fail_install(context, "staging_create_failed",
			"无法创建临时安装目录：" + error.message());
		return;
	}
	if (!vfs::mount("/dev_flash", staging_path + '/'))
	{
		std::filesystem::remove_all(staging_path, error);
		fail_install(context, "staging_mount_failed", "无法挂载临时固件目录到 /dev_flash");
		return;
	}

	for (const std::string& filename : filenames)
	{
		update_firmware_status(true, "extracting", "正在解密并安装固件组件",
			context.completed, context.total, context.version);
		auto stream = update_files.get_file(filename);
		if (!stream)
		{
			restore_live_mount(live_path);
			std::filesystem::remove_all(staging_path, error);
			fail_install(context, "package_read_failed", "无法读取固件组件 " + filename);
			return;
		}
		if (stream->m_file_handler)
		{
			stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
		}

		fs::file encrypted = fs::make_stream(std::move(stream->data));
		SCEDecrypter decrypter(encrypted);
		if (!decrypter.LoadHeaders() || !decrypter.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV) || !decrypter.DecryptData())
		{
			restore_live_mount(live_path);
			std::filesystem::remove_all(staging_path, error);
			fail_install(context, "decrypt_failed", "固件组件解密失败 " + filename);
			return;
		}

		auto files = decrypter.MakeFile();
		if (files.size() < 3 || !tar_object(files[2]).extract())
		{
			restore_live_mount(live_path);
			std::filesystem::remove_all(staging_path, error);
			fail_install(context, "extract_failed", "固件组件解包失败 " + filename);
			return;
		}

		context.completed++;
	}

	const bool has_version = std::filesystem::is_regular_file(staging_path + "/vsh/etc/version.txt", error);
	const bool has_vsh = std::filesystem::is_regular_file(staging_path + "/vsh/module/vsh.self", error);
	if (!has_version || !has_vsh)
	{
		restore_live_mount(live_path);
		std::filesystem::remove_all(staging_path, error);
		fail_install(context, "verification_failed", "解包后的固件缺少关键系统文件");
		return;
	}

	update_firmware_status(true, "committing", "固件校验通过，正在提交安装",
		context.completed, context.total, context.version);
	const bool had_live = std::filesystem::exists(live_path, error);
	if (had_live)
	{
		std::filesystem::rename(live_path, backup_path, error);
		if (error)
		{
			restore_live_mount(live_path);
			std::filesystem::remove_all(staging_path, error);
			fail_install(context, "backup_failed", "现有固件目录无法备份");
			return;
		}
	}

	std::filesystem::rename(staging_path, live_path, error);
	if (error)
	{
		if (had_live)
		{
			std::error_code restore_error;
			std::filesystem::rename(backup_path, live_path, restore_error);
		}
		restore_live_mount(live_path);
		fail_install(context, "commit_failed", "新固件提交失败，旧固件已保留");
		return;
	}

	restore_live_mount(live_path);
	if (had_live)
	{
		std::filesystem::remove_all(backup_path, error);
	}

	context.ok = true;
	context.code = "ok";
	context.message = "PS3 固件安装完成";
	update_firmware_status(false, "complete", context.message,
		context.completed, context.total, context.version);
}

void execute_firmware_install(napi_env, void* data)
{
	auto& context = *static_cast<firmware_install_context*>(data);
	run_firmware_install(context);
}

void complete_firmware_install(napi_env env, napi_status status, void* data)
{
	std::unique_ptr<firmware_install_context> context(static_cast<firmware_install_context*>(data));
	if (status != napi_ok && context->code == "unknown")
	{
		fail_install(*context, "async_failed", "固件安装后台任务执行失败");
	}

	napi_value result{};
	napi_create_object(env, &result);
	set_named(env, result, "ok", make_bool(env, context->ok));
	set_named(env, result, "code", make_string(env, context->code));
	set_named(env, result, "message", make_string(env, context->message));
	set_named(env, result, "version", make_string(env, context->version));
	set_named(env, result, "installedVersion", make_string(env, utils::get_firmware_version()));
	set_named(env, result, "completed", make_int(env, context->completed));
	set_named(env, result, "total", make_int(env, context->total));
	napi_resolve_deferred(env, context->deferred, result);
	napi_delete_async_work(env, context->work);
	g_install_active = false;
}

napi_value install_firmware(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		napi_throw_type_error(env, nullptr, "installFirmware requires a PUP path");
		return nullptr;
	}

	size_t length = 0;
	if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || length == 0)
	{
		napi_throw_type_error(env, nullptr, "installFirmware path must be a non-empty string");
		return nullptr;
	}

	bool expected = false;
	if (!g_install_active.compare_exchange_strong(expected, true))
	{
		napi_throw_error(env, nullptr, "firmware installation already active");
		return nullptr;
	}

	auto context = std::make_unique<firmware_install_context>();
	context->pup_path.resize(length + 1);
	if (napi_get_value_string_utf8(env, argv[0], context->pup_path.data(), context->pup_path.size(), &length) != napi_ok)
	{
		g_install_active = false;
		napi_throw_type_error(env, nullptr, "failed to read PUP path");
		return nullptr;
	}
	context->pup_path.resize(length);

	napi_value promise{};
	if (napi_create_promise(env, &context->deferred, &promise) != napi_ok)
	{
		g_install_active = false;
		return nullptr;
	}
	napi_value resource_name = make_string(env, "hps3.installFirmware");
	if (napi_create_async_work(env, nullptr, resource_name, execute_firmware_install,
		complete_firmware_install, context.get(), &context->work) != napi_ok)
	{
		g_install_active = false;
		return nullptr;
	}
	if (napi_queue_async_work(env, context->work) != napi_ok)
	{
		napi_delete_async_work(env, context->work);
		g_install_active = false;
		return nullptr;
	}
	context.release();
	return promise;
}

napi_value initialize(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		return make_bool(env, false);
	}

	size_t length = 0;
	if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || length == 0)
	{
		return make_bool(env, false);
	}

	std::string root(length + 1, '\0');
	if (napi_get_value_string_utf8(env, argv[0], root.data(), root.size(), &length) != napi_ok)
	{
		return make_bool(env, false);
	}
	root.resize(length);
	if (root.back() != '/')
	{
		root.push_back('/');
	}

	// HarmonyOS loads the core as a shared library, before ArkTS can provide
	// the per-application files directory. Use the same late-bound directories
	// as the Android frontend so fs::get_config_dir() cannot cache an unusable
	// process-relative path during static initialization.
	g_android_executable_dir = root;
	g_android_config_dir = root + "config/";
	g_android_cache_dir = root + "cache/";

	std::lock_guard lock(g_emu_mutex);
	if (g_initialized)
	{
		return make_bool(env, true);
	}
	if (!hps3_check_signal_abi()) return make_bool(env, false);

	std::error_code fs_error;
	std::filesystem::create_directories(root + "config", fs_error);
	if (fs_error)
	{
		return make_bool(env, false);
	}
	std::filesystem::create_directories(root + "cache", fs_error);
	if (fs_error)
	{
		return make_bool(env, false);
	}
	const std::string runtime_root = root + "rpcs3/";
	std::filesystem::create_directories(runtime_root, fs_error);
	if (fs_error)
	{
		return make_bool(env, false);
	}

	setenv("XDG_CONFIG_HOME", (root + "config").c_str(), 1);
	setenv("XDG_CACHE_HOME", (root + "cache").c_str(), 1);

	configure_headless_callbacks();
	Emu.SetHasGui(false);
	// Must stay false. Emulator::Load() runs fixup_settings() on every boot, and
	// that function force-downgrades the video renderer to Null whenever
	// Emu.IsHeadless() is true ("not supported in headless mode"). With headless
	// set, the Vulkan choice written to config.yml was silently replaced by Null
	// during Load, so init_gs_render() always built NullGSRender and no game frame
	// could ever reach the XComponent surface. Headless is irrelevant here: every
	// dialog/frame callback is supplied by this bridge, not by Qt.
	Emu.SetHeadless(false);
	Emu.SetUsr("00000001");
	Emu.SetSupportedRenderers({video_renderer::null, video_renderer::vulkan});
	// Vulkan is the real target; the RSX thread falls back to NullGSRender on its
	// own when no XComponent surface exists yet (see init_gs_render).
	Emu.SetDefaultRenderer(video_renderer::vulkan);
	Emu.SetDefaultGraphicsAdapter("Default");
	Emu.Init();
	g_cfg_input.load();
	g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
	g_cfg_input.player1.device.set("Virtual");
	g_cfg_input.save("", g_cfg_input_configs.default_config);
	g_cfg.video.renderer.set(video_renderer::vulkan);
	// The universal shader interpreter prewarms thousands of pipelines on this
	// driver before RSX can acknowledge a pause. Compile the game's shaders on
	// demand instead, using the normal async recompiler path.
	g_cfg.video.shadermode.set(shader_mode::async_recompiler);
	// Maleoon shares system RAM. Desktop's 64 GiB default prevents cache
	// eviction before HarmonyOS terminates the process (GPU_THRESHOLD_KILLER).
	// Leave room for the guest, JIT, driver pipelines and the rest of the OS.
	g_cfg.video.vk.vram_allocation_limit.set(1536);
	g_cfg.video.shader_compiler_threads_count.set(2);
	g_cfg.audio.renderer.set(audio_renderer::cubeb);
	g_cfg.audio.music.set(music_handler::null);
	g_cfg.io.keyboard.set(keyboard_handler::null);
	g_cfg.io.mouse.set(mouse_handler::null);
	g_cfg.misc.autostart.set(false);

	// fs::get_config_dir() may be cached by RPCS3 static initialization before
	// ArkTS can provide the application sandbox path. Pin every runtime VFS
	// entry after Init() and remount it under files/rpcs3 instead of relying on
	// that process-global cache.
	g_cfg_vfs.emulator_dir.set(runtime_root);
	g_cfg_vfs.dev_hdd0.set("$(EmulatorDir)dev_hdd0/");
	g_cfg_vfs.dev_hdd1.set("$(EmulatorDir)dev_hdd1/");
	g_cfg_vfs.dev_flash.set("$(EmulatorDir)dev_flash/");
	g_cfg_vfs.dev_flash2.set("$(EmulatorDir)dev_flash2/");
	g_cfg_vfs.dev_flash3.set("$(EmulatorDir)dev_flash3/");
	g_cfg_vfs.games_dir.set("$(EmulatorDir)games/");
	// BootGame() calls Emulator::Init() again. Persist the sandbox VFS and
	// headless backend choices so that second initialization does not revert to
	// RPCS3's desktop defaults and look for firmware outside the app sandbox.
	g_cfg_vfs.save();
	Emulator::SaveSettings(g_cfg.to_string(), {});

	// Must come before BootGame: anything logged earlier is discarded.
	install_log_listener();

	// Arm the fault probe here rather than in runGame(): the Vulkan backend is
	// constructed inside BootGame (init_gs_render), so a crash there would land
	// before the probe was ever installed and leave no forensics at all.
	install_segv_probe();

	vfs::mount("/dev_hdd0", g_cfg_vfs.get(g_cfg_vfs.dev_hdd0));
	vfs::mount("/dev_hdd1", g_cfg_vfs.get(g_cfg_vfs.dev_hdd1));
	vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());
	vfs::mount("/dev_flash2", g_cfg_vfs.get_dev_flash2());
	vfs::mount("/dev_flash3", g_cfg_vfs.get_dev_flash3());
	g_runtime_root = runtime_root;
	g_initialized = true;
	return make_bool(env, true);
}

napi_value inspect_game_image(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		return make_game_image_result(env, false, "invalid_argument", "需要系统文件选择器返回的 ISO 文件句柄");
	}

	int32_t source_fd = -1;
	if (napi_get_value_int32(env, argv[0], &source_fd) != napi_ok || source_fd < 0)
	{
		return make_game_image_result(env, false, "invalid_fd", "ISO 文件句柄无效");
	}

	std::lock_guard lock(g_game_image_mutex);
	if (!g_initialized || g_runtime_root.empty())
	{
		return make_game_image_result(env, false, "not_initialized", "模拟器核心尚未初始化");
	}

	const int duplicated_fd = ::dup(source_fd);
	if (duplicated_fd < 0)
	{
		return make_game_image_result(env, false, "dup_failed",
			std::string("无法保留 ISO 文件访问权限：") + std::strerror(errno));
	}

	const std::string game_dir = g_runtime_root + "selected_game/";
	std::error_code fs_error;
	std::filesystem::create_directories(game_dir, fs_error);
	if (fs_error)
	{
		::close(duplicated_fd);
		return make_game_image_result(env, false, "create_link_dir_failed", "无法创建游戏临时目录");
	}

	const std::string link_path = game_dir + "selected.iso";
	std::filesystem::remove(link_path, fs_error);
	fs_error.clear();
	const std::string fd_path = "/proc/self/fd/" + std::to_string(duplicated_fd);
	std::filesystem::create_symlink(fd_path, link_path, fs_error);
	// HarmonyOS application sandboxes may reject symlink creation even though
	// the retained descriptor itself is readable. Prefer the friendly .iso
	// link when permitted, otherwise read the duplicated descriptor directly.
	const std::string image_path = fs_error ? fd_path : link_path;

	if (g_game_image_fd >= 0)
	{
		::close(g_game_image_fd);
	}
	g_game_image_fd = duplicated_fd;
	g_game_image_path = image_path;

	{
		fs::file image(image_path, fs::read);
		if (!image)
		{
			return make_game_image_result(env, false, "open_failed", "核心无法打开所选 ISO", 0,
				false, false, false, {}, {}, {}, image_path);
		}

		const int64_t image_size = static_cast<int64_t>(image.size());
		u64 detected_size = 0;
		if (!is_iso_file(image_path, &detected_size))
		{
			return make_game_image_result(env, false, "not_iso", "文件没有有效的 ISO9660/PS3 光盘结构",
				image_size, false, false, false, {}, {}, {}, image_path);
		}

		iso_archive archive(image_path);
		if (!archive.is_valid())
		{
			return make_game_image_result(env, false, "unreadable_iso",
				"已识别 ISO，但无法读取 PS3 文件系统；镜像可能已加密、缺少匹配的 .dkey/.key，或文件不完整",
				image_size, false, false, false, {}, {}, {}, image_path);
		}

		const bool has_param_sfo = archive.is_file("PS3_GAME/PARAM.SFO");
		const bool has_eboot = archive.is_file("PS3_GAME/USRDIR/EBOOT.BIN");
		const psf::registry game_psf = has_param_sfo
			? archive.open_psf("PS3_GAME/PARAM.SFO")
			: psf::registry{};
		const std::string title = std::string(psf::get_string(game_psf, "TITLE", ""));
		const std::string title_id = std::string(psf::get_string(game_psf, "TITLE_ID", ""));
		const std::string category = std::string(psf::get_string(game_psf, "CATEGORY", ""));

		if (!has_param_sfo)
		{
			return make_game_image_result(env, false, "missing_param_sfo",
				"ISO 可读取，但没有找到 PS3_GAME/PARAM.SFO", image_size, true,
				false, has_eboot, title, title_id, category, image_path);
		}
		if (!has_eboot)
		{
			return make_game_image_result(env, false, "missing_eboot",
				"ISO 可读取，但没有找到 PS3_GAME/USRDIR/EBOOT.BIN；它可能是安装盘而非游戏盘",
				image_size, true, true, false, title, title_id, category, image_path);
		}
		if (title_id.empty())
		{
			return make_game_image_result(env, false, "invalid_param_sfo",
				"PARAM.SFO 存在，但缺少有效的 TITLE_ID", image_size, true,
				true, true, title, title_id, category, image_path);
		}

		return make_game_image_result(env, true, "ok", "已识别可读取的 PS3 游戏 ISO",
			image_size, true, true, true, title, title_id, category, image_path);
	}
}

napi_value boot_game(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		napi_value result{};
		napi_create_int32(env, static_cast<int32_t>(game_boot_result::generic_error), &result);
		return result;
	}

	size_t length = 0;
	if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || length == 0)
	{
		napi_value result{};
		napi_create_int32(env, static_cast<int32_t>(game_boot_result::invalid_file_or_folder), &result);
		return result;
	}

	std::string path(length + 1, '\0');
	if (napi_get_value_string_utf8(env, argv[0], path.data(), path.size(), &length) != napi_ok)
	{
		napi_value result{};
		napi_create_int32(env, static_cast<int32_t>(game_boot_result::invalid_file_or_folder), &result);
		return result;
	}
	path.resize(length);

	std::lock_guard lock(g_emu_mutex);
	napi_value result{};
	const auto boot_result = g_initialized
		? Emu.BootGame(path, "", false, cfg_mode::custom)
		: game_boot_result::generic_error;
	napi_create_int32(env, static_cast<int32_t>(boot_result), &result);
	return result;
}

struct boot_context
{
	napi_async_work work{};
	napi_deferred deferred{};
	std::string path;
	int32_t result = static_cast<int32_t>(game_boot_result::generic_error);
};

napi_value boot_game_async(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	size_t length = 0;
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1 ||
		napi_get_value_string_utf8(env, argv[0], nullptr, 0, &length) != napi_ok || !length)
	{
		napi_throw_type_error(env, nullptr, "expected a game image path");
		return nullptr;
	}
	auto context = std::make_unique<boot_context>();
	context->path.resize(length + 1);
	napi_get_value_string_utf8(env, argv[0], context->path.data(), context->path.size(), &length);
	context->path.resize(length);
	napi_value promise{};
	if (napi_create_promise(env, &context->deferred, &promise) != napi_ok)
		return nullptr;
	const auto execute = [](napi_env, void* data)
	{
		auto& context = *static_cast<boot_context*>(data);
		std::lock_guard lock(g_emu_mutex);
		context.result = static_cast<int32_t>(g_initialized
			? Emu.BootGame(context.path, "", false, cfg_mode::custom)
			: game_boot_result::generic_error);
		install_log_listener();
	};
	const auto complete = [](napi_env env, napi_status status, void* data)
	{
		std::unique_ptr<boot_context> context(static_cast<boot_context*>(data));
		if (status != napi_ok)
		{
			napi_value error{};
			napi_create_error(env, nullptr, make_string(env, "game boot cancelled"), &error);
			napi_reject_deferred(env, context->deferred, error);
		}
		else
			napi_resolve_deferred(env, context->deferred, make_int(env, context->result));
		napi_delete_async_work(env, context->work);
	};
	if (napi_create_async_work(env, nullptr, make_string(env, "hps3.bootGame"), execute,
		complete, context.get(), &context->work) != napi_ok)
		return nullptr;
	if (napi_queue_async_work(env, context->work) != napi_ok)
	{
		napi_delete_async_work(env, context->work);
		return nullptr;
	}
	context.release();
	return promise;
}

// T9: leave Ready for guest execution.
//
// BootGame() intentionally stops at Ready (g_cfg.misc.autostart stays false) so
// the boot chain remains independently verifiable. Emulator::Run() is the
// upstream entry that moves the state machine to `starting`; from there the RSX
// thread observes Emu.IsStarting() and signals RunPPU(), and the main PPU thread
// calls FinalizeRunRequest() once the LV2 scheduler is ready -- which is what
// finally sets `running`. Run() returns immediately; it does not block until the
// game exits.
void install_segv_probe();
void dump_self_maps();

napi_value run_game(napi_env env, napi_callback_info)
{
	std::lock_guard lock(g_emu_mutex);

	napi_value result{};
	napi_create_object(env, &result);

	if (!g_initialized)
	{
		set_named(env, result, "ok", make_bool(env, false));
		set_named(env, result, "code", make_string(env, "not_initialized"));
		set_named(env, result, "message", make_string(env, "模拟器核心尚未初始化"));
		set_named(env, result, "state", make_int(env, static_cast<int32_t>(Emu.GetStatus())));
		return result;
	}

	if (!Emu.IsReady())
	{
		set_named(env, result, "ok", make_bool(env, false));
		set_named(env, result, "code", make_string(env, "not_ready"));
		set_named(env, result, "message", make_string(env, "核心不在 Ready 状态，需先成功装载游戏"));
		set_named(env, result, "state", make_int(env, static_cast<int32_t>(Emu.GetStatus())));
		return result;
	}

	HPS3_LOG("HPS3_RUN_BEGIN renderer=%d frames=%llu", g_active_renderer.load(),
		static_cast<unsigned long long>(g_frame_count.load()));

	// Arm the fault probe before guest code can run: the PPU thread is about to
	// be released, and the first SIGSEGV currently kills the process.
	install_segv_probe();

	// autostart must be false while Load() runs so the boot chain parks at Ready
	// (that is what makes T8 independently verifiable), but it must be true by
	// the time FinalizeRunRequest() reads it -- otherwise it CASes to running and
	// then immediately Pause()s back to `paused`. It is a dynamic cfg entry, so
	// flipping it here between Load and Run is safe.
	g_cfg.misc.autostart.set(true);

	Emu.Run(true);

	// RSX releases the PPU after backend initialization, as in the upstream
	// startup sequence. A second frontend RunPPU races that release and can
	// reset the stop flag twice while the guest is still compiling.

	const int32_t state = static_cast<int32_t>(Emu.GetStatus());
	HPS3_LOG("HPS3_RUN_ISSUED state=%d", state);

	set_named(env, result, "ok", make_bool(env, true));
	set_named(env, result, "code", make_string(env, "ok"));
	set_named(env, result, "message", make_string(env,
		state == 7 ? "已进入 Starting，等待 guest 线程就绪" : "Run() 已下发"));
	set_named(env, result, "state", make_int(env, state));
	return result;
}

napi_value set_pad_button(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value argv[2]{};
	int32_t index = -1;
	bool pressed = false;
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 2 ||
		napi_get_value_int32(env, argv[0], &index) != napi_ok ||
		napi_get_value_bool(env, argv[1], &pressed) != napi_ok)
		return make_bool(env, false);
	std::lock_guard lock(pad::g_pad_mutex);
	auto* handler = pad::get_pad_thread(true);
	if (!handler || !pad::g_started) return make_bool(env, false);
	const auto& controller = handler->GetPads()[0];
	if (!controller || controller->m_pad_handler != pad_handler::virtual_pad)
		return make_bool(env, false);

	if (index == -1 && !pressed)
	{
		for (auto& button : controller->m_buttons)
		{
			button.m_pressed = false;
			button.m_value = 0;
		}
		for (auto& stick : controller->m_sticks) stick.m_value = 128;
	}
	else if (index >= 0 && static_cast<size_t>(index) < controller->m_buttons.size())
	{
		auto& button = controller->m_buttons[index];
		button.m_pressed = pressed;
		button.m_value = pressed ? 255 : 0;
	}
	else if (index >= 16 && index <= 19)
	{
		// Separate movement keys drive the left analog stick, leaving the D-pad
		// available for the game's independent menu/weapon controls.
		controller->m_sticks[index < 18 ? 1 : 0].m_value =
			pressed ? ((index == 16 || index == 18) ? 0 : 255) : 128;
	}
	else return make_bool(env, false);
	HPS3_LOG("HPS3_PAD button=%{public}d pressed=%{public}d", index, pressed ? 1 : 0);
	return make_bool(env, true);
}

napi_value set_pad_stick(napi_env env, napi_callback_info info)
{
	size_t argc = 3;
	int32_t stick = 0;
	int32_t x = 128;
	int32_t y = 128;
	napi_value args[3]{};
	if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok || argc != 3 ||
		napi_get_value_int32(env, args[0], &stick) != napi_ok ||
		napi_get_value_int32(env, args[1], &x) != napi_ok ||
		napi_get_value_int32(env, args[2], &y) != napi_ok ||
		(stick != 0 && stick != 1))
		return make_bool(env, false);

	std::lock_guard lock(pad::g_pad_mutex);
	auto* handler = pad::get_pad_thread(true);
	if (!handler || !pad::g_started) return make_bool(env, false);
	const auto& controller = handler->GetPads()[0];
	if (!controller || controller->m_pad_handler != pad_handler::virtual_pad ||
		controller->m_sticks.size() < 4)
		return make_bool(env, false);

	const size_t axis = static_cast<size_t>(stick) * 2;
	controller->m_sticks[axis].m_value = static_cast<u16>(std::clamp(x, 0, 255));
	controller->m_sticks[axis + 1].m_value = static_cast<u16>(std::clamp(y, 0, 255));
	return make_bool(env, true);
}

// T9 crash forensics.
//
// The PPU thread dies with SIGSEGV/si_code=SEGV_ACCERR the moment guest code
// starts, and HarmonyOS will not hand out /data/log/faultlog (permission
// denied), so the platform's own backtrace is unreachable from here. This
// handler captures the three values that actually identify the fault before
// chaining to the previously installed action:
//   si_addr - the faulting address, which tells us whether the guest touched a
//             protected JIT page or an unmapped guest range
//   PC      - whether the fault is inside generated code or in the runtime
//   LR      - the caller, to disambiguate the JIT entry path
// Async-signal-safe by construction: only OH_LOG_Print and a plain int store.
std::atomic<uint64_t> g_fault_addr{0};
std::atomic<uint64_t> g_fault_pc{0};
std::atomic<uint64_t> g_fault_lr{0};
std::atomic<int32_t> g_fault_count{0};
std::atomic<int32_t> g_execute_fault_count{0};

struct sigaction g_prev_segv {};
bool g_segv_installed = false;

void hps3_segv_handler(int sig, siginfo_t* info, void* ucontext)
{
	const int32_t n = g_fault_count.fetch_add(1);
	siginfo_t* const d = info;
	uint64_t pc_v = 0;
	uint64_t lr_v = 0;
	if (info)
	{
		g_fault_addr.store(reinterpret_cast<uint64_t>(info->si_addr));
	}
	if (ucontext)
	{
		auto* uc = static_cast<ucontext_t*>(ucontext);
		pc_v = static_cast<uint64_t>(uc->uc_mcontext.pc);
		lr_v = static_cast<uint64_t>(uc->uc_mcontext.regs[30]);
		g_fault_pc.store(pc_v);
		g_fault_lr.store(lr_v);
	}
	// hilog masks every pointer as "<private>", so the log line alone cannot
	// identify the fault. Persist the raw values to the app sandbox where they
	// survive the process and can be pulled with hdc.
	// open/write are async-signal-safe; no allocation, no locale, no locking.
	const bool instruction_fault = pc_v != 0 && info &&
		pc_v == reinterpret_cast<uintptr_t>(info->si_addr);
	const int32_t execute_index = instruction_fault ? g_execute_fault_count.fetch_add(1) : -1;
	if (n < 8 || (instruction_fault && execute_index < 8))
	{
		// One-shot full mapping table: pinpoints which region (and its protection
		// bits) actually contains pc.
		if (n == 0 || execute_index == 0)
		{
			dump_self_maps();
			// write() safely returns EFAULT for an unreadable range; avoid directly
			// dereferencing a possibly invalid return address inside the handler.
			const int code_fd = ::open("/data/storage/el2/base/files/hps3_jit_lr.bin",
				O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (code_fd >= 0)
			{
				(void)!::write(code_fd, reinterpret_cast<const void*>(lr_v - 128), 256);
				::close(code_fd);
			}
			if (auto* ppu = cpu_thread::get_current<ppu_thread>())
			{
				const int guest_fd = ::open("/data/storage/el2/base/files/hps3_guest_cia.bin",
					O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (guest_fd >= 0)
				{
					(void)!::write(guest_fd, vm::g_base_addr + ppu->cia - 64, 256);
					::close(guest_fd);
				}
			}
		}

		// Also record the JIT region bounds and the loaded module base, so the
		// faulting address can be attributed without /proc (which the sandbox
		// denies) and without guessing from si_addr alone.
		const uintptr_t jit_lo = reinterpret_cast<uintptr_t>(jit_runtime::peek(true));
		const uintptr_t jit_hi = jit_lo + 0x80000000ull;
		Dl_info dli{};
		const uintptr_t so_base = dladdr(reinterpret_cast<void*>(pc_v), &dli)
			? reinterpret_cast<uintptr_t>(dli.dli_fbase) : 0ull;
		// Symbol name is far more actionable than a raw offset when the mapping
		// does not line up with a guessed load bias.
		const char* sym = (dli.dli_sname && dli.dli_sname[0]) ? dli.dli_sname : "(nosym)";
		const uintptr_t sym_off = reinterpret_cast<uintptr_t>(dli.dli_saddr);
		Dl_info dli_lr{};
		const char* sym_lr = dladdr(reinterpret_cast<void*>(lr_v), &dli_lr)
			? ((dli_lr.dli_sname && dli_lr.dli_sname[0]) ? dli_lr.dli_sname : "(nosym)")
			: "(none)";

		// Dump the full register file. The faulting target lives in some register
		// (an indirect branch), and knowing which one -- plus the words around LR
		// in the JIT page -- identifies the generated instruction without a
		// disassembler round trip.
		char buf[1600];
		int len = snprintf(buf, sizeof(buf),
			"HPS3_SEGV #%d sig=%d addr=0x%llx pc=0x%llx lr=0x%llx jit=0x%llx-0x%llx so=0x%llx sym=%s+0x%llx lrsym=%s\n",
			n, sig,
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(d ? d->si_addr : nullptr)),
			static_cast<unsigned long long>(pc_v),
			static_cast<unsigned long long>(lr_v),
			static_cast<unsigned long long>(jit_lo),
			static_cast<unsigned long long>(jit_hi),
			static_cast<unsigned long long>(so_base),
			sym, static_cast<unsigned long long>(pc_v - sym_off), sym_lr);

		if (ucontext && len > 0 && len < static_cast<int>(sizeof(buf)))
		{
			auto* uc = static_cast<ucontext_t*>(ucontext);
			int added = snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len), "REGS");
			if (added > 0) len += added;
			for (int i = 0; i < 31 && len < static_cast<int>(sizeof(buf)) - 24; i++)
			{
				added = snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len),
					" x%d=0x%llx", i,
					static_cast<unsigned long long>(uc->uc_mcontext.regs[i]));
				if (added <= 0) break;
				len += added;
			}
			if (len < static_cast<int>(sizeof(buf)) - 24)
			{
				added = snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len),
					" sp=0x%llx\n",
					static_cast<unsigned long long>(uc->uc_mcontext.sp));
				if (added > 0) len += added;
			}
		}
		if (len > 0)
		{
			const int fd = ::open("/data/storage/el2/base/files/hps3_segv.log",
				O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0)
			{
				(void)!::write(fd, buf, static_cast<size_t>(len));
				::close(fd);
			}
		}
	}

	// Chain, so the platform still produces its own crash record.
	if (g_prev_segv.sa_flags & SA_SIGINFO && g_prev_segv.sa_sigaction)
	{
		g_prev_segv.sa_sigaction(sig, info, ucontext);
	}
}

void install_segv_probe()
{
	if (g_segv_installed)
	{
		return;
	}

	struct sigaction sa {};
	sa.sa_sigaction = hps3_segv_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGSEGV, &sa, &g_prev_segv) == 0)
	{
		g_segv_installed = true;
		HPS3_LOG("HPS3_SEGV_PROBE installed");
	}
}

// Returns the persisted SIGSEGV probe log. hilog masks pointers and the file is
// mode 0600 under the app uid, so the page reads it through this call instead.
napi_value read_fault_log(napi_env env, napi_callback_info)
{
	std::string text;
	if (FILE* f = std::fopen("/data/storage/el2/base/files/hps3_segv.log", "re"))
	{
		char buf[512];
		size_t got = 0;
		while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
		{
			text.append(buf, got);
			if (text.size() > 4096)
			{
				break;
			}
		}
		std::fclose(f);
	}
	return make_string(env, text);
}

napi_value dump_maps(napi_env env, napi_callback_info)
{
	dump_self_maps();
	return make_bool(env, true);
}

// The sandbox denies /proc/<other-pid>/maps, but a process may always read its
// own. Dumping it is the only way to attribute a faulting address to a concrete
// mapping with its protection bits -- guessing from si_addr alone already led
// one conclusion astray.
void dump_self_maps()
{
	const int in = ::open("/proc/self/maps", O_RDONLY);
	if (in < 0)
	{
		return;
	}
	const int out = ::open("/data/storage/el2/base/files/hps3_maps.txt",
		O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out >= 0)
	{
		char buf[4096];
		ssize_t got = 0;
		size_t total = 0;
		while ((got = ::read(in, buf, sizeof(buf))) > 0 && total < (2u << 20))
		{
			(void)!::write(out, buf, static_cast<size_t>(got));
			total += static_cast<size_t>(got);
		}
		::close(out);
	}
	::close(in);
}

// T9: one consolidated evidence snapshot.
//
// getState() alone cannot distinguish "thread objects exist" from "guest code is
// retiring instructions" -- a live process looks identical either way. This
// reports the state machine plus independent counters so each stage is judged
// from device-side numbers instead of inference.
napi_value get_runtime_status(napi_env env, napi_callback_info)
{
	napi_value result{};
	napi_create_object(env, &result);

	set_named(env, result, "state", make_int(env, static_cast<int32_t>(Emu.GetStatus())));
	set_named(env, result, "running", make_bool(env, Emu.IsRunning()));
	set_named(env, result, "ready", make_bool(env, Emu.IsReady()));
	set_named(env, result, "starting", make_bool(env, Emu.IsStarting()));
	set_named(env, result, "paused", make_bool(env, Emu.IsPaused()));
	set_named(env, result, "frames", make_int64(env, static_cast<int64_t>(g_frame_count.load())));
	HPS3_LOG("HPS3_VM_FAULTS recovered=%{public}llu", static_cast<unsigned long long>(g_ohos_recovered_faults.load()));
	HPS3_LOG("HPS3_AUDIO_DATA callbacks=%{public}llu bytes=%{public}llu nonzero=%{public}llu",
		static_cast<unsigned long long>(hps3_audio_callbacks.load()),
		static_cast<unsigned long long>(hps3_audio_bytes.load()),
		static_cast<unsigned long long>(hps3_audio_nonzero.load()));
	set_named(env, result, "renderer", make_int(env, g_active_renderer.load()));
	set_named(env, result, "runCalls", make_int(env, g_run_calls.load()));
	set_named(env, result, "readyCalls", make_int(env, g_ready_calls.load()));
	set_named(env, result, "stopCalls", make_int(env, g_stop_calls.load()));

	// Guest thread census. PPU/SPU counts prove the guest threads were created;
	// the summed instruction pointers let two samples show that they advanced.
	int32_t ppu_count = 0;
	int32_t spu_count = 0;
	uint64_t ppu_cia_sum = 0;
	uint64_t spu_pc_sum = 0;
	// Thread flags decide whether the PPU is actively waiting on a guest object
	// (dbg_global_pause / suspend / wait) or has run off into a stopped state.
	// cif is not enough: a thread parked on a never-signalled lv2 object freezes
	// its cia exactly like a finished one does.
	uint32_t ppu_flags = 0;
	int32_t ppu_host_state = -1;
	int32_t ppu_errored = 0;
	std::string ppu_details;
	std::string spu_details;

	idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& cpu)
	{
		ppu_count++;
		ppu_cia_sum += cpu.cia;
		ppu_flags = static_cast<uint32_t>(+cpu.state);
		ppu_host_state = static_cast<int32_t>(static_cast<thread_state>(cpu));
		ppu_errored += static_cast<thread_state>(cpu) == thread_state::errored;
		fmt::append(ppu_details, "id=%x cia=%x flags=%x host=%d lr=%llx fn=%s; ",
			id, cpu.cia, static_cast<u32>(+cpu.state), ppu_host_state, cpu.lr,
			cpu.current_function ? cpu.current_function : (cpu.last_function ? cpu.last_function : "-"));
	});
	idm::select<named_thread<spu_thread>>([&](u32 id, named_thread<spu_thread>& spu)
	{
		spu_count++;
		spu_pc_sum += spu.pc;
		fmt::append(spu_details, "id=%x pc=%x flags=%x host=%d; ", id, static_cast<u32>(spu.pc),
			static_cast<u32>(+spu.state), static_cast<int32_t>(static_cast<thread_state>(spu)));
	});

	set_named(env, result, "ppuThreads", make_int(env, ppu_count));
	set_named(env, result, "spuThreads", make_int(env, spu_count));
	set_named(env, result, "ppuCiaSum", make_int64(env, static_cast<int64_t>(ppu_cia_sum)));
	set_named(env, result, "spuPcSum", make_int64(env, static_cast<int64_t>(spu_pc_sum)));
	set_named(env, result, "ppuFlags", make_int64(env, static_cast<int64_t>(ppu_flags)));
	set_named(env, result, "ppuHostState", make_int(env, ppu_host_state));
	set_named(env, result, "ppuErroredThreads", make_int(env, ppu_errored));
	set_named(env, result, "ppuDetail", make_string(env, ppu_details));
	set_named(env, result, "spuDetail", make_string(env, spu_details));

	// The PPU parks in `while (u32 v = g_progr_ptotal) ... wait(v)` before it
	// starts running guest code. A compile worker that dies mid-module never
	// decrements this, so it never reaches zero and the PPU waits forever --
	// which looks exactly like a frozen cia with cpu_flag == 0.
	set_named(env, result, "progTotal", make_int64(env, static_cast<int64_t>(+g_progr_ptotal)));
	set_named(env, result, "progDone", make_int64(env, static_cast<int64_t>(+g_progr_pdone)));

	// Guest-side facts that separate "still booting" from "parked forever":
	// lr/sp show whether the thread is inside a call, and status shows whether
	// LV2 still considers this thread runnable.
	{
		uint64_t ppu_lr = 0;
		uint64_t ppu_sp = 0;
		idm::select<named_thread<ppu_thread>>([&](u32, named_thread<ppu_thread>& cpu)
		{
			ppu_lr = cpu.lr;
			ppu_sp = cpu.gpr[1];
		});
		set_named(env, result, "ppuLr", make_int64(env, static_cast<int64_t>(ppu_lr)));
		set_named(env, result, "ppuSp", make_int64(env, static_cast<int64_t>(ppu_sp)));
	}

	// Guest-side position of the PPU thread that faulted. The guest CIA is what
	// ties a native fault back to the emulated instruction, which the native PC
	// alone cannot do.
	{
		uint32_t guest_cia = 0;
		idm::select<named_thread<ppu_thread>>([&](u32, named_thread<ppu_thread>& cpu)
		{
			guest_cia = cpu.cia;
		});
		set_named(env, result, "guestCia", make_int64(env, guest_cia));
	}

	// Crash forensics from the SIGSEGV probe, so the faulting address and PC can
	// be read back even though the process is torn down moments later.
	napi_value fault{};
	napi_create_object(env, &fault);
	set_named(env, fault, "count", make_int(env, g_fault_count.load()));
	set_named(env, fault, "executeCount", make_int(env, g_execute_fault_count.load()));
	set_named(env, fault, "addr", make_int64(env, static_cast<int64_t>(g_fault_addr.load())));
	set_named(env, fault, "pc", make_int64(env, static_cast<int64_t>(g_fault_pc.load())));
	set_named(env, fault, "lr", make_int64(env, static_cast<int64_t>(g_fault_lr.load())));
	set_named(env, result, "fault", fault);
	return result;
}

// T9 bisect switch: choose PPU/SPU decoders before booting.
//
// The PPU dies with SIGSEGV the instant guest code runs, and the fault is a
// fetch into the jemalloc heap (native pc), i.e. generated code jumping to a bad
// target. Running the same title on the interpreter separates "the recompiler
// emits a bad branch" from "something below both backends (memory/ABI) is
// wrong", which is the difference between a JIT bug and a port bug.
napi_value set_decoders(napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value argv[2]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 2)
	{
		return make_bool(env, false);
	}

	int32_t ppu_mode = 1;
	int32_t spu_mode = 1;
	napi_get_value_int32(env, argv[0], &ppu_mode);
	napi_get_value_int32(env, argv[1], &spu_mode);

	std::lock_guard lock(g_emu_mutex);
	if (Emu.IsRunning() || Emu.IsStarting() || Emu.IsPaused())
	{
		HPS3_LOG("HPS3_SET_DECODERS rejected state=%d", static_cast<int32_t>(Emu.GetStatus()));
		return make_bool(env, false);
	}

	g_cfg.core.ppu_decoder.set(ppu_mode == 0 ? ppu_decoder_type::_static : ppu_decoder_type::llvm);
	g_cfg.core.spu_decoder.set(spu_mode == 0 ? spu_decoder_type::_static
		: (spu_mode == 2 ? spu_decoder_type::asmjit : spu_decoder_type::llvm));

	// Load() reloads config.yml, so this must be persisted too.
	Emulator::SaveSettings(g_cfg.to_string(), {});
	HPS3_LOG("HPS3_SET_DECODERS ppu=%d spu=%d", ppu_mode, spu_mode);
	return make_bool(env, true);
}

// Select the RSX backend before booting. Null isolates guest execution from the
// renderer (T9 stage A); Vulkan is required for a visible game frame (stage B).
napi_value set_renderer(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		return make_bool(env, false);
	}

	int32_t mode = 0;
	if (napi_get_value_int32(env, argv[0], &mode) != napi_ok)
	{
		return make_bool(env, false);
	}

	std::lock_guard lock(g_emu_mutex);

	// Changing the renderer with a session in flight would desync
	// init_gs_render() from the backend the RSX thread already built against.
	if (Emu.IsRunning() || Emu.IsStarting() || Emu.IsPaused())
	{
		HPS3_LOG("HPS3_SET_RENDERER rejected state=%d", static_cast<int32_t>(Emu.GetStatus()));
		return make_bool(env, false);
	}

	g_cfg.video.renderer.set((mode == 1) ? video_renderer::vulkan : video_renderer::null);
	// Must persist: Emulator::Load() reloads config.yml from disk on every boot,
	// so an in-memory-only change would be reverted before init_gs_render() runs.
	Emulator::SaveSettings(g_cfg.to_string(), {});
	HPS3_LOG("HPS3_SET_RENDERER mode=%d", mode);
	return make_bool(env, true);
}

// Keep the RSX internal resolution selectable for device A/B measurements.
// The value must be chosen before BootGame because RSX caches its scaling
// configuration when the renderer is created.
napi_value get_resolution_scale(napi_env env, napi_callback_info)
{
	std::lock_guard lock(g_emu_mutex);
	if (!g_initialized)
	{
		return make_int(env, 100);
	}
	return make_int(env, static_cast<int32_t>(g_cfg.video.resolution_scale_percent.get()));
}

napi_value set_resolution_scale(napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1]{};
	if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 1)
	{
		return make_bool(env, false);
	}

	int32_t percent = 100;
	if (napi_get_value_int32(env, argv[0], &percent) != napi_ok ||
		(percent != 100 && percent != 75 && percent != 50))
	{
		return make_bool(env, false);
	}

	std::lock_guard lock(g_emu_mutex);
	if (!g_initialized || Emu.IsRunning() || Emu.IsStarting() || Emu.IsPaused())
	{
		HPS3_LOG("HPS3_SET_RESOLUTION_SCALE rejected state=%d initialized=%d",
			static_cast<int32_t>(Emu.GetStatus()), g_initialized);
		return make_bool(env, false);
	}

	g_cfg.video.resolution_scale_percent.set(percent);
	Emulator::SaveSettings(g_cfg.to_string(), {});
	HPS3_LOG("HPS3_SET_RESOLUTION_SCALE percent=%d", percent);
	return make_bool(env, true);
}

napi_value register_module(napi_env env, napi_value exports)
{
	napi_property_descriptor properties[] = {
		{"isCoreLinked", nullptr, is_core_linked, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getState", nullptr, get_state, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"initialize", nullptr, initialize, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getInstalledFirmwareVersion", nullptr, get_installed_firmware_version, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getFirmwareInstallStatus", nullptr, get_firmware_install_status, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"installFirmware", nullptr, install_firmware, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getSurfaceStatus", nullptr, get_surface_status, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"probeVulkanSurface", nullptr, probe_vulkan_surface, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"presentVulkanTestPattern", nullptr, present_vulkan_test_pattern, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"inspectGameImage", nullptr, inspect_game_image, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"bootGame", nullptr, boot_game, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"bootGameAsync", nullptr, boot_game_async, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"runGame", nullptr, run_game, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getRuntimeStatus", nullptr, get_runtime_status, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setRenderer", nullptr, set_renderer, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setDecoders", nullptr, set_decoders, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"getResolutionScale", nullptr, get_resolution_scale, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setResolutionScale", nullptr, set_resolution_scale, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setPadButton", nullptr, set_pad_button, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"setPadStick", nullptr, set_pad_stick, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"readFaultLog", nullptr, read_fault_log, nullptr, nullptr, nullptr, napi_default, nullptr},
		{"dumpMaps", nullptr, dump_maps, nullptr, nullptr, nullptr, napi_default, nullptr},
	};
	napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
	register_xcomponent(env, exports);
	return exports;
}

napi_module g_module = {
	.nm_version = NAPI_MODULE_VERSION,
	.nm_flags = 0,
	.nm_filename = nullptr,
	.nm_register_func = register_module,
	.nm_modname = "hps3core",
	.nm_priv = nullptr,
	.reserved = {nullptr, nullptr, nullptr, nullptr},
};
} // namespace

extern "C" __attribute__((constructor)) void register_hps3_core_module()
{
	napi_module_register(&g_module);
}
