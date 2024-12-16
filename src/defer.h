#ifndef DEFER_INCLUDED
#define DEFER_INCLUDED

struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(COUNTER) zz_defer##COUNTER
#define DEFER(COUNTER) DEFER_(COUNTER)

#define defer auto DEFER(__COUNTER__) = defer_dummy{} *[&]()


#endif // DEFER_INCLUDED
