#include "foobar2000.h"
#include "foosort.h"

namespace {
	using namespace fb2k;

	class callOnReleaseImpl : public service_base {
    public:
        callOnReleaseImpl( std::function<void () > f_) : f(f_) {}
        std::function<void ()> f;
        
        ~callOnReleaseImpl () {
            try {
                f();
            } catch(...) {}
        }
    };
}

namespace fb2k {
    objRef callOnRelease( std::function< void () > f) {
        return new service_impl_t< callOnReleaseImpl > (f);
    }
    objRef callOnReleaseInMainThread( std::function< void () > f) {
        return callOnRelease( [f] {
           fb2k::inMainThread2( f );
        });
    }
}

namespace pfc {
	/*
	Redirect PFC methods to shared.dll
	If you're getting linker multiple-definition errors on these, change build configuration of PFC from "Debug" / "Release" to "Debug FB2K" / "Release FB2K"
	*/
	BOOL winFormatSystemErrorMessageHook(pfc::string_base & p_out, DWORD p_code) {
		return uFormatSystemErrorMessage(p_out, p_code);
	}
	void crashHook() {
		uBugCheck();
	}
}


// file_lock_manager.h functionality
#include "file_lock_manager.h"
namespace {
    class file_lock_interrupt_impl : public file_lock_interrupt {
    public:
        void interrupt( abort_callback & a ) { f(a); }
        std::function<void (abort_callback&)> f;
    };
}

file_lock_interrupt::ptr file_lock_interrupt::create( std::function< void (abort_callback&)> f ) {
    service_ptr_t<file_lock_interrupt_impl> i = new service_impl_t<file_lock_interrupt_impl>();
    i->f = f;
    return i;
}
