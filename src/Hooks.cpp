#include "Hooks.h"

#include "InputManager.h"
#include "Keycode.h"
#include "ui/Menu.h"
#include "ui/MenuHost.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace {
std::atomic_bool g_windowShutdownObserved{false};
std::mutex g_wndProcMapMutex;
std::unordered_map<ATOM, WNDPROC> g_originalWndProcsByAtom;

std::atomic_bool g_menuFrameworkBridgeRegistered{false};
std::atomic_bool g_menuFrameworkCallbackSeen{false};
std::int64_t g_menuFrameworkEventId{-1};

constexpr int kMenuFrameworkBeforeRender = 3;
constexpr int kMenuFrameworkAfterRender = 4;
using MenuFrameworkEventCallback = void(__stdcall *)(int);
using RegisterMenuFrameworkEvent =
    std::int64_t (*)(MenuFrameworkEventCallback, float);

void __stdcall OnMenuFrameworkEvent(const int a_eventType) {
  if (g_windowShutdownObserved.load(std::memory_order_relaxed)) {
    return;
  }

  if (!g_menuFrameworkCallbackSeen.exchange(true, std::memory_order_relaxed)) {
    logger::info(
        "SVS Build13 fix: SKSE Menu Framework render callback is active");
  }

  if (a_eventType == kMenuFrameworkBeforeRender) {
    // Build 13 can replace/redirect Skyrim's old present path. Process the
    // queued SVS hotkey/input from the Menu Framework render lifecycle instead
    // so opening the menu no longer depends on our legacy present hook.
    sosr::InputManager::GetSingleton()->ProcessInputEvents();
    return;
  }

  if (a_eventType != kMenuFrameworkAfterRender) {
    return;
  }

  auto *menu = sosr::Menu::GetSingleton();
  if (!menu->IsEnabled()) {
    return;
  }

  // Build 13 explicitly supports SKSE Menu Framework's UI render path. Draw
  // SVS while that path is active and keep the render target selected by the
  // framework/upscaler instead of forcing the swapchain backbuffer.
  menu->Draw();
}

bool TryRegisterMenuFrameworkBridge() {
  if (g_menuFrameworkBridgeRegistered.load(std::memory_order_relaxed)) {
    return true;
  }

  const auto module = GetModuleHandleW(L"SKSEMenuFramework.dll");
  if (module == nullptr) {
    logger::warn(
        "SVS Build13 fix: SKSEMenuFramework.dll not loaded; using legacy "
        "present fallback");
    return false;
  }

  const auto registerEvent = reinterpret_cast<RegisterMenuFrameworkEvent>(
      GetProcAddress(module, "RegisterEventPriority"));
  if (registerEvent == nullptr) {
    logger::warn(
        "SVS Build13 fix: SKSE Menu Framework does not export "
        "RegisterEventPriority; using legacy present fallback");
    return false;
  }

  g_menuFrameworkEventId = registerEvent(&OnMenuFrameworkEvent, 1000.0f);
  if (g_menuFrameworkEventId < 0) {
    logger::warn(
        "SVS Build13 fix: failed to register SKSE Menu Framework event "
        "callback; using legacy present fallback");
    return false;
  }

  g_menuFrameworkBridgeRegistered.store(true, std::memory_order_relaxed);
  logger::info(
      "SVS Build13 fix: registered SKSE Menu Framework render bridge id={}",
      g_menuFrameworkEventId);
  return true;
}

auto GetOriginalWndProc(HWND a_hwnd) -> WNDPROC {
  const auto atom =
      static_cast<ATOM>(::GetClassLongPtrA(a_hwnd, GCW_ATOM) & 0xFFFF);
  if (atom == 0) {
    return nullptr;
  }

  std::scoped_lock lock(g_wndProcMapMutex);
  if (const auto it = g_originalWndProcsByAtom.find(atom);
      it != g_originalWndProcsByAtom.end()) {
    return it->second;
  }

  return nullptr;
}

[[nodiscard]] bool ShouldBlockMenuInputEvent(const RE::InputEvent *a_event) {
  if (a_event == nullptr || !sosr::Menu::GetSingleton()->IsEnabled()) {
    return false;
  }

  if (a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton ||
      a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
    return false;
  }

  const auto *buttonEvent = a_event->AsButtonEvent();
  return buttonEvent != nullptr &&
         buttonEvent->GetIDCode() == sosr::keycode::kTabScanCode;
}

void FilterBlockedInputEvents(RE::InputEvent **a_events) {
  if (a_events == nullptr) {
    return;
  }

  auto **link = a_events;
  while (*link != nullptr) {
    auto *event = *link;
    if (ShouldBlockMenuInputEvent(event)) {
      *link = event->next;
      continue;
    }

    link = &event->next;
  }
}

static void
hk_PollInputDevices(RE::BSTEventSource<RE::InputEvent *> *a_dispatcher,
                    RE::InputEvent **a_events);
static inline REL::Relocation<decltype(hk_PollInputDevices)> g_inputHandler;
static inline REL::Relocation<uintptr_t> g_registerClass{
    REL::VariantID(75591, 77226, 0xDC4B90)};

void hk_PollInputDevices(RE::BSTEventSource<RE::InputEvent *> *a_dispatcher,
                         RE::InputEvent **a_events) {
  if (a_events) {
    sosr::InputManager::GetSingleton()->AddEventToQueue(a_events);
    FilterBlockedInputEvents(a_events);
  }

  g_inputHandler(a_dispatcher, a_events);
}

struct WndProcHook {
  static LRESULT thunk(HWND a_hwnd, UINT a_msg, WPARAM a_wParam,
                       LPARAM a_lParam) {
    switch (a_msg) {
    case WM_CLOSE:
      if (!g_windowShutdownObserved.exchange(true, std::memory_order_relaxed)) {
        sosr::Menu::GetSingleton()->NotifyWindowShutdown();
      }
      break;
    case WM_DESTROY:
    case WM_NCDESTROY:
      g_windowShutdownObserved.store(true, std::memory_order_relaxed);
      break;
    case WM_ACTIVATE: {
      const auto activationType = LOWORD(a_wParam);
      if (activationType != WA_INACTIVE) {
        sosr::InputManager::GetSingleton()->Flush();
        sosr::InputManager::GetSingleton()->OnFocusChange(true);
      }
      break;
    }
    case WM_SETFOCUS:
      sosr::InputManager::GetSingleton()->Flush();
      sosr::InputManager::GetSingleton()->OnFocusChange(true);
      break;
    case WM_KILLFOCUS:
      sosr::InputManager::GetSingleton()->OnFocusChange(false);
      break;
    default:
      break;
    }

    const auto originalWndProc = GetOriginalWndProc(a_hwnd);
    if (originalWndProc == nullptr) {
      logger::warn("SVS hook: missing original WndProc hwnd={} msg=0x{:X}",
                   static_cast<void *>(a_hwnd), a_msg);
      return DefWindowProcA(a_hwnd, a_msg, a_wParam, a_lParam);
    }

    return CallWindowProcA(originalWndProc, a_hwnd, a_msg, a_wParam, a_lParam);
  }
};

struct RegisterClassAHook {
  static ATOM thunk(WNDCLASSA *a_wndClass) {
    const auto originalWndProc = a_wndClass->lpfnWndProc;
    a_wndClass->lpfnWndProc = &WndProcHook::thunk;
    const auto atom = func(a_wndClass);
    if (atom != 0 && originalWndProc != nullptr) {
      std::scoped_lock lock(g_wndProcMapMutex);
      g_originalWndProcsByAtom[atom] = originalWndProc;
    }
    return atom;
  }

  static inline REL::Relocation<decltype(thunk)> func;
};
} // namespace

namespace sosr::hooks {
bool IsWindowShutdownObserved() {
  return g_windowShutdownObserved.load(std::memory_order_relaxed);
}

struct D3DInitHook {
  static void thunk() {
    func();

    auto *renderer = RE::BSGraphics::Renderer::GetSingleton();
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(
        renderer->GetRuntimeData().context);
    auto *swapChain = reinterpret_cast<IDXGISwapChain *>(
        renderer->GetRuntimeData().renderWindows->swapChain);
    auto *device =
        reinterpret_cast<ID3D11Device *>(renderer->GetRuntimeData().forwarder);

    Menu::GetSingleton()->Init(swapChain, device, context);
    MenuHost::RegisterMenu();
    TryRegisterMenuFrameworkBridge();
  }

  static inline REL::Relocation<decltype(thunk)> func;
};

struct PresentHook {
  static void thunk(std::uint32_t a_argument) {
    func(a_argument);
    if (g_windowShutdownObserved.load(std::memory_order_relaxed)) {
      return;
    }

    // On Build 13 we use SKSE Menu Framework's supported UI render lifecycle.
    // Keep the old path only as a fallback for setups without the framework.
    if (!g_menuFrameworkBridgeRegistered.load(std::memory_order_relaxed)) {
      InputManager::GetSingleton()->ProcessInputEvents();
      Menu::GetSingleton()->Draw();
    }
  }

  static inline REL::Relocation<decltype(thunk)> func;
};

void Install() {
  auto &trampoline = SKSE::GetTrampoline();

  logger::info("Hooking BSInputDeviceManager::PollInputDevices");
  g_inputHandler =
      trampoline.write_call<5>(REL::RelocationID(67315, 68617).address() +
                                   REL::Relocate(0x7B, 0x7B, 0x81),
                               hk_PollInputDevices);

  logger::info("Hooking RegisterClassA");
  const auto registerClassTarget = trampoline.write_call<6>(
      g_registerClass.address() +
          REL::VariantOffset(0x8E, 0x15C, 0x99).offset(),
      RegisterClassAHook::thunk);
  if (registerClassTarget == 0) {
    logger::critical("Failed to hook RegisterClassA");
    return;
  }
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  RegisterClassAHook::func =
      *reinterpret_cast<const uintptr_t *>(registerClassTarget);

  logger::info("Hooking BSGraphics::Renderer::InitD3D");
  D3DInitHook::func = trampoline.write_call<5>(
      REL::RelocationID(75595, 77226).address() + REL::Relocate(0x50, 0x2BC),
      D3DInitHook::thunk);

  logger::info("Hooking DXGI present");
  PresentHook::func =
      trampoline.write_call<5>(REL::RelocationID(75461, 77246).address() +
                                   REL::VariantOffset(0x9, 0x9, 0x15).offset(),
                               PresentHook::thunk);
}
} // namespace sosr::hooks
