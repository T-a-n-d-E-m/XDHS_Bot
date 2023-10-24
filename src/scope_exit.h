#ifndef SCOPE_EXIT_INCLUDED
#define SCOPE_EXIT_INCLUDED

// http://the-witness.net/news/2012/11/scopeexit-in-c11/
template<typename F>
struct ScopeExit {
	ScopeExit(F f_) : f(f_) {}
	~ScopeExit() {
		f();
	}
	F f;
};

template <typename F>
ScopeExit<F> MakeScopeExit(F f) {
	return ScopeExit<F>(f);
}
#define SCOPE_EXIT_JOIN_STRING_2(arg1, arg2) SCOPE_EXIT_DO_JOIN_STRING_2(arg1, arg2)
#define SCOPE_EXIT_DO_JOIN_STRING_2(arg1, arg2) arg1 ## arg2
#define SCOPE_EXIT(code) auto SCOPE_EXIT_JOIN_STRING_2(scope_exit_, __LINE__) = MakeScopeExit([=](){code;})

#endif // SCOPE_EXIT_INCLUDED
