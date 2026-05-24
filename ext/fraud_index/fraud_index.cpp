// Ruby C++ extension: expõe FraudIndex.load(path) e FraudIndex.score(vec14).

#include "ivf_index.hpp"
#include <ruby.h>

using namespace ivf;

// Singleton: o index é carregado uma vez na boot da API e fica vivo até o
// processo morrer. Em produção a API tem um único worker Puma, então sem
// concorrência. Se mudar pra cluster mode, repensar.
static IvfIndex g_index;
static bool     g_loaded  = false;
static int      g_nprobe  = 1;

extern "C" {

// FraudIndex.load(path) → true/false
static VALUE rb_fraud_index_load(VALUE self, VALUE path) {
  (void)self;
  Check_Type(path, T_STRING);

  if (g_loaded) {
    unload_index(g_index);
    g_loaded = false;
  }

  if (load_index(StringValueCStr(path), g_index)) {
    g_loaded = true;
    return Qtrue;
  }
  return Qfalse;
}

// FraudIndex.score(query_array_de_14_floats) → Float (fraud_score)
static VALUE rb_fraud_index_score(VALUE self, VALUE arr) {
  (void)self;
  Check_Type(arr, T_ARRAY);

  if (!g_loaded) {
    rb_raise(rb_eRuntimeError, "FraudIndex: índice não carregado (chame .load primeiro)");
  }
  if (RARRAY_LEN(arr) != static_cast<long>(DIM)) {
    rb_raise(rb_eArgError, "FraudIndex.score: query precisa ter %u elementos, recebeu %ld",
             DIM, RARRAY_LEN(arr));
  }

  float query[DIM];
  for (uint32_t i = 0; i < DIM; ++i) {
    query[i] = static_cast<float>(NUM2DBL(rb_ary_entry(arr, i)));
  }

  float score = ivf_score(g_index, query, g_nprobe);
  return DBL2NUM(score);
}

// FraudIndex.loaded? → true/false
static VALUE rb_fraud_index_loaded(VALUE self) {
  (void)self;
  return g_loaded ? Qtrue : Qfalse;
}

// FraudIndex.nprobe → Integer
static VALUE rb_fraud_index_get_nprobe(VALUE self) {
  (void)self;
  return INT2NUM(g_nprobe);
}

// FraudIndex.nprobe = N
static VALUE rb_fraud_index_set_nprobe(VALUE self, VALUE n) {
  (void)self;
  int v = NUM2INT(n);
  if (v < 1) rb_raise(rb_eArgError, "nprobe deve ser >= 1");
  g_nprobe = v;
  return INT2NUM(v);
}

void Init_fraud_index(void) {
  VALUE mod = rb_define_module("FraudIndex");
  rb_define_singleton_method(mod, "load",     RUBY_METHOD_FUNC(rb_fraud_index_load),       1);
  rb_define_singleton_method(mod, "score",    RUBY_METHOD_FUNC(rb_fraud_index_score),      1);
  rb_define_singleton_method(mod, "loaded?",  RUBY_METHOD_FUNC(rb_fraud_index_loaded),     0);
  rb_define_singleton_method(mod, "nprobe",   RUBY_METHOD_FUNC(rb_fraud_index_get_nprobe), 0);
  rb_define_singleton_method(mod, "nprobe=",  RUBY_METHOD_FUNC(rb_fraud_index_set_nprobe), 1);
}

} // extern "C"
