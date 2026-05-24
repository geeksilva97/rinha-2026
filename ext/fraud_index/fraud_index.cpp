// Ruby C++ extension: exposes FraudIndex.load(path) and FraudIndex.score(vec14).

#include "ivf_index.hpp"
#include <ruby.h>
#include <ruby/thread.h>

using namespace ivf;

// Arguments + result for the GVL-released scoring call.
struct ScoreCall {
  const IvfIndex *idx;
  const float    *query;
  int             nprobe;
  float           result;
};

// Runs ivf_score outside the Ruby GVL so other Puma threads can run in
// parallel during the kNN search (the hot CPU-bound path).
// CRITICAL: do NOT call any Ruby C API from here.
static void *score_no_gvl(void *data) {
  auto *c = static_cast<ScoreCall *>(data);
  c->result = ivf_score(*c->idx, c->query, c->nprobe);
  return nullptr;
}

// Singleton: the index is loaded once at API boot and lives until the
// process exits. In prod the API runs a single Puma worker, so no
// concurrency. Revisit if we move to cluster mode.
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

// FraudIndex.score(query_array_of_14_floats) → Float (fraud_score)
static VALUE rb_fraud_index_score(VALUE self, VALUE arr) {
  (void)self;
  Check_Type(arr, T_ARRAY);

  if (!g_loaded) {
    rb_raise(rb_eRuntimeError, "FraudIndex: index not loaded (call .load first)");
  }
  if (RARRAY_LEN(arr) != static_cast<long>(DIM)) {
    rb_raise(rb_eArgError, "FraudIndex.score: query needs %u elements, got %ld",
             DIM, RARRAY_LEN(arr));
  }

  float query[DIM];
  for (uint32_t i = 0; i < DIM; ++i) {
    query[i] = static_cast<float>(NUM2DBL(rb_ary_entry(arr, i)));
  }

  // Release the GVL during the compute-heavy kNN search.
  // Other Puma threads can run their Ruby code in parallel.
  ScoreCall call = { &g_index, query, g_nprobe, 0.0f };
  rb_thread_call_without_gvl(score_no_gvl, &call, RUBY_UBF_IO, nullptr);
  return DBL2NUM(call.result);
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
  if (v < 1) rb_raise(rb_eArgError, "nprobe must be >= 1");
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
