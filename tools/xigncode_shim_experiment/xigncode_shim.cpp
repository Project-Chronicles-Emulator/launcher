#include <cstdarg>
#include <cstdint>
#include <windows.h>

namespace {

constexpr std::int32_t kSuccess = 0;
constexpr std::int32_t kNoInterface = static_cast<std::int32_t>(0x80004002u);
constexpr std::int32_t kInvalidPointer = static_cast<std::int32_t>(0x80004003u);
constexpr std::int32_t kOriginalFailStub = static_cast<std::int32_t>(0xE0000001u);

constexpr std::uint32_t kInitializeMethod = 1;
constexpr std::uint32_t kLazyMethod = 2;
constexpr std::uint32_t kRegisterCallbackMethod = 22;
constexpr std::uint32_t kVariadicCommandMethod = 26;
constexpr std::uint32_t kBootstrapMethod = 34;

constexpr std::uint32_t kStartCommand = 200;
constexpr std::uint32_t kRegisterQueryCommand = 0x55;
constexpr std::uint32_t kRegisterClientCommand = 0x1BBC;

// The game tests this as a Boolean. No runtime state is created by this experiment.
std::uint32_t InitializeRuntime(
    std::uintptr_t host_context,
    const wchar_t* module_path,
    std::uint32_t flags) noexcept
{
    static_cast<void>(host_context);
    static_cast<void>(module_path);
    static_cast<void>(flags);
    return 1;
}

// The game checks this result as a signed status value.
std::int32_t CompleteBootstrap(
    void* bootstrap_context,
    void* reserved_first,
    void* reserved_second) noexcept
{
    static_cast<void>(bootstrap_context);
    static_cast<void>(reserved_first);
    static_cast<void>(reserved_second);
    return kSuccess;
}

std::int32_t InvokeLazyMethod() noexcept
{
    return kSuccess;
}

void RegisterCallback(void (*callback)(), std::uintptr_t context) noexcept
{
    static_cast<void>(callback);
    static_cast<void>(context);
}

// The game's wrapper passes its va_list address. Its command-specific arguments
// are not interpreted until their ABI and ownership are established.
std::uint32_t DispatchCommand(std::uint32_t command, std::va_list* arguments) noexcept
{
    static_cast<void>(arguments);

    switch (command) {
        case kStartCommand:
        case kRegisterQueryCommand:
        case kRegisterClientCommand:
            return 1;
        default:
            return 0;
    }
}

template <typename Function>
std::int32_t ProvideMethod(void** output, Function function) noexcept
{
    if (output == nullptr) {
        return kInvalidPointer;
    }

    *output = reinterpret_cast<void*>(function);
    return kSuccess;
}

}  // namespace

extern "C" {

unsigned char X3Ordinal99ZeroPage[4096] = {};

std::int32_t X3ResolveOrdinal1(void** output, std::uint32_t method_id) noexcept
{
    if (output == nullptr) {
        return kInvalidPointer;
    }

    *output = nullptr;

    switch (method_id) {
        case kInitializeMethod:
            return ProvideMethod(output, &InitializeRuntime);
        case kLazyMethod:
            return ProvideMethod(output, &InvokeLazyMethod);
        case kRegisterCallbackMethod:
            return ProvideMethod(output, &RegisterCallback);
        case kVariadicCommandMethod:
            return ProvideMethod(output, &DispatchCommand);
        default:
            return kNoInterface;
    }
}

std::int32_t X3ResolveOrdinal4(void** output, std::uint32_t method_id) noexcept
{
    if (output == nullptr) {
        return kInvalidPointer;
    }

    *output = nullptr;

    if (method_id == kBootstrapMethod) {
        return ProvideMethod(output, &CompleteBootstrap);
    }

    return X3ResolveOrdinal1(output, method_id);
}

std::int32_t X3FailStub2() noexcept
{
    return kOriginalFailStub;
}

std::int32_t X3FailStub3() noexcept
{
    return kOriginalFailStub;
}

}  // extern "C"
