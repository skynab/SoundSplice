#if defined(__linux__)

#include <dlfcn.h>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

/*
    A test binary's X errors are not fatal.

    JUCE creates a real X window for every top-level component - an
    AlertWindow among them - and sets window-manager properties on it as it
    does. Under xvfb there is no window manager, so atoms like WM_PROTOCOLS
    were never created, JUCE looks them up as 0, and the server answers the
    property change with BadAtom. In the app that is nothing: JUCE installs an
    X error handler that ignores it. But it installs that handler only for a
    standalone app (XWindowSystem::initialiseXDisplay), which a test binary
    isn't, so Xlib's default handler takes it instead - and that one prints
    the error and exits the process, failing the test with no output of its
    own.

    So the same handler JUCE would have installed: ignore the error, carry on.
    Looked up from libX11 at run time, as JUCE itself does, rather than
    linked, so the test binary needs nothing at build time it didn't before.
*/
namespace
{
    int ignoreXError(void*, void*) { return 0; }

    class XErrorListener final : public Catch::EventListenerBase
    {
    public:
        using Catch::EventListenerBase::EventListenerBase;

        void testRunStarting(const Catch::TestRunInfo&) override
        {
            using Handler = int (*)(void*, void*);
            using SetHandler = Handler (*)(Handler);

            if (auto* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL))
                if (auto set = reinterpret_cast<SetHandler>(dlsym(x11, "XSetErrorHandler")))
                    set(ignoreXError);
        }
    };
}

CATCH_REGISTER_LISTENER(XErrorListener)

#endif
