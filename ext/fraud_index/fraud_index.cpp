// Ruby C++ extension: exposes the IVF index plus an alloc-free JSON parser
// specialized for the rinha payload schema.
//
//   FraudIndex.load(path)              → bool
//   FraudIndex.score(vec14)            → Float (fraud_count / 5.0)
//   FraudIndex.fraud_count(vec14)      → Integer (0..5)
//   FraudIndex.fraud_count_payload(b)  → Integer (0..5)   ← parse + score in C
//   FraudIndex.nprobe / nprobe=
//   FraudIndex.loaded?

#include "ivf_index.hpp"
#include <ruby.h>
#include <ctime>
#include <cstdint>

using namespace ivf;

// ─── Index singleton ─────────────────────────────────────────────────────────
static IvfIndex g_index;
static bool     g_loaded = false;
static int      g_nprobe = 1;

// ─── Payload parser ──────────────────────────────────────────────────────────
namespace {

constexpr uint8_t QUOTE  = 34;
constexpr uint8_t COMMA  = 44;
constexpr uint8_t COLON  = 58;
constexpr uint8_t LBRACE = 123;
constexpr uint8_t RBRACE = 125;
constexpr uint8_t LBRACK = 91;
constexpr uint8_t RBRACK = 93;
constexpr uint8_t DOT    = 46;
constexpr uint8_t MINUS  = 45;
constexpr uint8_t D0     = 48;
constexpr uint8_t D9     = 57;
constexpr uint8_t SP     = 32;
constexpr uint8_t HT     = 9;
constexpr uint8_t LF     = 10;
constexpr uint8_t CR     = 13;

constexpr float INV_MAX_AMOUNT       = 1.0f / 10000.0f;
constexpr float INV_MAX_INSTALLMENTS = 1.0f / 12.0f;
constexpr float INV_AMOUNT_VS_AVG    = 1.0f / 10.0f;
constexpr float INV_HOUR             = 1.0f / 23.0f;
constexpr float INV_DOW              = 1.0f / 6.0f;
constexpr float INV_MAX_MINUTES      = 1.0f / 1440.0f;
constexpr float INV_MAX_KM           = 1.0f / 1000.0f;
constexpr float INV_MAX_TX_COUNT     = 1.0f / 20.0f;
constexpr float INV_MAX_MERCHANT_AVG = 1.0f / 10000.0f;

static constexpr int DOW_TABLE[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

static inline float mcc_risk(uint32_t packed) {
  // Packed = (b0<<24)|(b1<<16)|(b2<<8)|b3 of the 4-digit MCC string.
  switch (packed) {
    case 0x35343131u: return 0.15f; // "5411"
    case 0x35383132u: return 0.30f; // "5812"
    case 0x35393132u: return 0.20f; // "5912"
    case 0x35393434u: return 0.45f; // "5944"
    case 0x37383031u: return 0.80f; // "7801"
    case 0x37383032u: return 0.75f; // "7802"
    case 0x37393935u: return 0.85f; // "7995"
    case 0x34353131u: return 0.35f; // "4511"
    case 0x35333131u: return 0.25f; // "5311"
    case 0x35393939u: return 0.50f; // "5999"
    default:          return 0.50f;
  }
}

static inline int sakamoto_dow(int y, int m, int d) {
  int ya = m < 3 ? y - 1 : y;
  int sak = (ya + ya / 4 - ya / 100 + ya / 400 + DOW_TABLE[m - 1] + d) % 7;
  return (sak + 6) % 7;
}

static inline float clamp01f(float v) {
  return v > 1.0f ? 1.0f : (v < 0.0f ? 0.0f : v);
}

struct Cur {
  const uint8_t *p;
  size_t         len;
  size_t         pos;

  inline uint8_t peek() const { return p[pos]; }
  inline bool    at_end() const { return pos >= len; }

  inline void skip_ws() {
    while (pos < len) {
      uint8_t b = p[pos];
      if (b != SP && b != HT && b != LF && b != CR) return;
      pos++;
    }
  }
  inline void skip_ws_comma() {
    while (pos < len) {
      uint8_t b = p[pos];
      if (b != SP && b != HT && b != LF && b != CR && b != COMMA) return;
      pos++;
    }
  }
  inline void skip_to_value() {
    while (pos < len && p[pos] != COLON) pos++;
    if (pos < len) pos++;
    skip_ws();
  }
  inline void scan_until_quote() {
    while (pos < len && p[pos] != QUOTE) pos++;
  }
  // Reads key bytes (opening quote already consumed). Returns [s, e), advances
  // past the closing quote.
  inline void read_key(size_t &s, size_t &e) {
    s = pos;
    while (pos < len && p[pos] != QUOTE) pos++;
    e = pos;
    if (pos < len) pos++;
  }

  inline double parse_number() {
    bool neg = false;
    uint8_t b = p[pos];
    if (b == MINUS) { neg = true; pos++; }
    int64_t ip = 0;
    while (pos < len) {
      b = p[pos];
      if (b < D0 || b > D9) break;
      ip = ip * 10 + (b - D0);
      pos++;
    }
    double r = static_cast<double>(ip);
    if (pos < len && p[pos] == DOT) {
      pos++;
      int64_t fr = 0, div = 1;
      while (pos < len) {
        b = p[pos];
        if (b < D0 || b > D9) break;
        fr = fr * 10 + (b - D0);
        div *= 10;
        pos++;
      }
      r += static_cast<double>(fr) / static_cast<double>(div);
    }
    return neg ? -r : r;
  }

  inline bool parse_bool() {
    bool v = (p[pos] == 't');
    pos += v ? 4 : 5;
    return v;
  }

  // Parses ISO "YYYY-MM-DDTHH:MM:SSZ". Caller ensures peek() == '"'.
  // Advances past the closing quote.
  inline void parse_iso(int &y, int &m, int &d, int &h, int &mi, int &se) {
    size_t s = pos + 1;
    y  = (p[s]     - 48) * 1000 + (p[s + 1]  - 48) * 100 +
         (p[s + 2] - 48) * 10   + (p[s + 3]  - 48);
    m  = (p[s + 5] - 48) * 10   + (p[s + 6]  - 48);
    d  = (p[s + 8] - 48) * 10   + (p[s + 9]  - 48);
    h  = (p[s + 11] - 48) * 10  + (p[s + 12] - 48);
    mi = (p[s + 14] - 48) * 10  + (p[s + 15] - 48);
    se = (p[s + 17] - 48) * 10  + (p[s + 18] - 48);
    pos = s + 21; // past 'Z' and closing quote
  }

  inline void skip_value() {
    skip_ws();
    if (at_end()) return;
    uint8_t b = peek();
    if (b == QUOTE) {
      pos++; scan_until_quote(); if (pos < len) pos++;
    } else if (b == LBRACE) {
      int depth = 1; pos++;
      while (pos < len && depth > 0) {
        uint8_t bb = p[pos];
        if      (bb == QUOTE) { pos++; scan_until_quote(); }
        else if (bb == LBRACE) depth++;
        else if (bb == RBRACE) depth--;
        pos++;
      }
    } else if (b == LBRACK) {
      int depth = 1; pos++;
      while (pos < len && depth > 0) {
        uint8_t bb = p[pos];
        if      (bb == QUOTE) { pos++; scan_until_quote(); }
        else if (bb == LBRACK) depth++;
        else if (bb == RBRACK) depth--;
        pos++;
      }
    } else if (b == 't') { pos += 4; }
    else if (b == 'f') { pos += 5; }
    else if (b == 'n') { pos += 4; }
    else {
      if (p[pos] == MINUS) pos++;
      while (pos < len) {
        uint8_t bb = p[pos];
        if (!((bb >= D0 && bb <= D9) || bb == DOT)) break;
        pos++;
      }
    }
  }
};

// Check if id-bytes match any string inside the known_merchants array bytes.
static bool id_in_known(const uint8_t *body, size_t id_s, size_t id_e,
                        size_t km_s, size_t km_e) {
  if (id_e <= id_s) return false;
  size_t id_len = id_e - id_s;
  size_t i = km_s;
  while (i < km_e) {
    if (body[i] == QUOTE) {
      i++;
      size_t s = i;
      while (i < km_e && body[i] != QUOTE) i++;
      if ((i - s) == id_len) {
        bool match = true;
        for (size_t j = 0; j < id_len; ++j) {
          if (body[s + j] != body[id_s + j]) { match = false; break; }
        }
        if (match) return true;
      }
      if (i < km_e) i++;
    } else {
      i++;
    }
  }
  return false;
}

// Walks the rinha payload bytes and fills a 14-float feature vector.
void parse_payload(const char *raw, size_t len, float *out) {
  out[0]=0; out[1]=0; out[2]=0; out[3]=0; out[4]=0;
  out[5]=-1.0f; out[6]=-1.0f;
  out[7]=0; out[8]=0; out[9]=0; out[10]=0;
  out[11]=1.0f; out[12]=0.5f; out[13]=0;

  Cur c{reinterpret_cast<const uint8_t *>(raw), len, 0};
  const uint8_t *body = c.p;

  float amount = 0.0f, cust_avg = 0.0f;
  int tx_y=0, tx_mo=0, tx_d=0, tx_h=0, tx_mi=0, tx_se=0;
  int lt_y=0, lt_mo=0, lt_d=0, lt_h=0, lt_mi=0, lt_se=0;
  float lt_km = 0.0f;
  bool  has_lt = false;
  size_t mid_s=0, mid_e=0;
  size_t km_s=0,  km_e=0;
  uint32_t mcc_packed = 0;

  c.skip_ws();
  if (!c.at_end() && c.peek() == LBRACE) c.pos++;

  while (!c.at_end()) {
    c.skip_ws_comma();
    if (c.at_end() || c.peek() == RBRACE) break;
    if (c.peek() != QUOTE) { c.pos++; continue; }
    c.pos++;
    size_t ks, ke;
    c.read_key(ks, ke);
    c.skip_to_value();
    size_t kl = ke - ks;
    uint8_t b0 = body[ks];

    if (kl == 11 && b0 == 't') {            // "transaction"
      c.skip_ws();
      if (c.peek() == LBRACE) c.pos++;
      while (!c.at_end() && c.peek() != RBRACE) {
        c.skip_ws_comma();
        if (c.peek() == RBRACE) break;
        if (c.peek() != QUOTE) { c.pos++; continue; }
        c.pos++;
        size_t sks, ske; c.read_key(sks, ske); c.skip_to_value();
        size_t skl = ske - sks; uint8_t sb0 = body[sks];
        if      (skl == 6  && sb0 == 'a') { amount = (float)c.parse_number(); out[0] = clamp01f(amount * INV_MAX_AMOUNT); }
        else if (skl == 12 && sb0 == 'i') { float inst = (float)c.parse_number(); out[1] = clamp01f(inst * INV_MAX_INSTALLMENTS); }
        else if (skl == 12 && sb0 == 'r') {
          c.parse_iso(tx_y, tx_mo, tx_d, tx_h, tx_mi, tx_se);
          out[3] = (float)tx_h * INV_HOUR;
          out[4] = (float)sakamoto_dow(tx_y, tx_mo, tx_d) * INV_DOW;
        }
        else c.skip_value();
      }
      if (c.pos < len) c.pos++;
    }
    else if (kl == 8 && b0 == 'c') {        // "customer"
      c.skip_ws();
      if (c.peek() == LBRACE) c.pos++;
      while (!c.at_end() && c.peek() != RBRACE) {
        c.skip_ws_comma();
        if (c.peek() == RBRACE) break;
        if (c.peek() != QUOTE) { c.pos++; continue; }
        c.pos++;
        size_t sks, ske; c.read_key(sks, ske); c.skip_to_value();
        size_t skl = ske - sks; uint8_t sb0 = body[sks];
        if      (skl == 10 && sb0 == 'a') { cust_avg = (float)c.parse_number(); }
        else if (skl == 12 && sb0 == 't') { float n = (float)c.parse_number(); out[8] = clamp01f(n * INV_MAX_TX_COUNT); }
        else if (skl == 15 && sb0 == 'k') {
          km_s = c.pos; c.skip_value(); km_e = c.pos;
        }
        else c.skip_value();
      }
      if (c.pos < len) c.pos++;
    }
    else if (kl == 8 && b0 == 'm') {        // "merchant"
      c.skip_ws();
      if (c.peek() == LBRACE) c.pos++;
      while (!c.at_end() && c.peek() != RBRACE) {
        c.skip_ws_comma();
        if (c.peek() == RBRACE) break;
        if (c.peek() != QUOTE) { c.pos++; continue; }
        c.pos++;
        size_t sks, ske; c.read_key(sks, ske); c.skip_to_value();
        size_t skl = ske - sks; uint8_t sb0 = body[sks];
        if      (skl == 2  && sb0 == 'i') {
          if (c.peek() == QUOTE) {
            c.pos++; mid_s = c.pos; c.scan_until_quote(); mid_e = c.pos;
            if (c.pos < len) c.pos++;
          }
        }
        else if (skl == 3  && sb0 == 'm') {
          if (c.peek() == QUOTE) {
            c.pos++; size_t ms = c.pos; c.scan_until_quote(); size_t me = c.pos;
            if (c.pos < len) c.pos++;
            if (me - ms == 4) {
              mcc_packed = (uint32_t)body[ms]     << 24 |
                           (uint32_t)body[ms + 1] << 16 |
                           (uint32_t)body[ms + 2] << 8  |
                           (uint32_t)body[ms + 3];
            }
          }
        }
        else if (skl == 10 && sb0 == 'a') { float mavg = (float)c.parse_number(); out[13] = clamp01f(mavg * INV_MAX_MERCHANT_AVG); }
        else c.skip_value();
      }
      if (c.pos < len) c.pos++;
    }
    else if (kl == 8 && b0 == 't') {        // "terminal"
      c.skip_ws();
      if (c.peek() == LBRACE) c.pos++;
      while (!c.at_end() && c.peek() != RBRACE) {
        c.skip_ws_comma();
        if (c.peek() == RBRACE) break;
        if (c.peek() != QUOTE) { c.pos++; continue; }
        c.pos++;
        size_t sks, ske; c.read_key(sks, ske); c.skip_to_value();
        size_t skl = ske - sks; uint8_t sb0 = body[sks];
        if      (skl == 9  && sb0 == 'i') { out[9]  = c.parse_bool() ? 1.0f : 0.0f; }
        else if (skl == 12 && sb0 == 'c') { out[10] = c.parse_bool() ? 1.0f : 0.0f; }
        else if (skl == 12 && sb0 == 'k') { float n = (float)c.parse_number(); out[7] = clamp01f(n * INV_MAX_KM); }
        else c.skip_value();
      }
      if (c.pos < len) c.pos++;
    }
    else if (kl == 16 && b0 == 'l') {       // "last_transaction"
      if (c.peek() == 'n') {                // null
        c.pos += 4; has_lt = false;
      } else {
        has_lt = true;
        c.skip_ws();
        if (c.peek() == LBRACE) c.pos++;
        while (!c.at_end() && c.peek() != RBRACE) {
          c.skip_ws_comma();
          if (c.peek() == RBRACE) break;
          if (c.peek() != QUOTE) { c.pos++; continue; }
          c.pos++;
          size_t sks, ske; c.read_key(sks, ske); c.skip_to_value();
          size_t skl = ske - sks; uint8_t sb0 = body[sks];
          if      (skl == 9  && sb0 == 't') { c.parse_iso(lt_y, lt_mo, lt_d, lt_h, lt_mi, lt_se); }
          else if (skl == 15 && sb0 == 'k') { lt_km = (float)c.parse_number(); }
          else c.skip_value();
        }
        if (c.pos < len) c.pos++;
      }
    }
    else {
      c.skip_value();
    }
  }

  if (cust_avg > 0.0f) {
    out[2] = clamp01f((amount / cust_avg) * INV_AMOUNT_VS_AVG);
  }

  if (has_lt) {
    struct tm tt = {tx_se, tx_mi, tx_h, tx_d, tx_mo - 1, tx_y - 1900, 0, 0, 0};
    struct tm lt = {lt_se, lt_mi, lt_h, lt_d, lt_mo - 1, lt_y - 1900, 0, 0, 0};
    time_t t_now  = timegm(&tt);
    time_t t_prev = timegm(&lt);
    float delta_min = static_cast<float>(t_now - t_prev) / 60.0f;
    out[5] = clamp01f(delta_min * INV_MAX_MINUTES);
    out[6] = clamp01f(lt_km     * INV_MAX_KM);
  }

  if (mid_e > mid_s && id_in_known(body, mid_s, mid_e, km_s, km_e)) {
    out[11] = 0.0f;
  }
  out[12] = mcc_risk(mcc_packed);
}

// Counts how many of the top-5 nearest references are labeled fraud.
// Effectively ivf_score(...) * 5 but returned as an int 0..5.
int fraud_count(const IvfIndex &idx, const float *query, int nprobe) {
  return static_cast<int>(ivf_score(idx, query, nprobe) * static_cast<float>(TOP_K) + 0.5f);
}

} // anon namespace

// ─── Ruby bindings ───────────────────────────────────────────────────────────
extern "C" {

static VALUE rb_fraud_index_load(VALUE self, VALUE path) {
  (void)self;
  Check_Type(path, T_STRING);
  if (g_loaded) { unload_index(g_index); g_loaded = false; }
  if (load_index(StringValueCStr(path), g_index)) {
    g_loaded = true;
    return Qtrue;
  }
  return Qfalse;
}

static VALUE rb_fraud_index_score(VALUE self, VALUE arr) {
  (void)self;
  Check_Type(arr, T_ARRAY);
  if (!g_loaded) rb_raise(rb_eRuntimeError, "FraudIndex: index not loaded");
  if (RARRAY_LEN(arr) != static_cast<long>(DIM))
    rb_raise(rb_eArgError, "FraudIndex.score: need %u floats, got %ld", DIM, RARRAY_LEN(arr));
  float q[DIM];
  for (uint32_t i = 0; i < DIM; ++i)
    q[i] = static_cast<float>(NUM2DBL(rb_ary_entry(arr, i)));
  return DBL2NUM(ivf_score(g_index, q, g_nprobe));
}

// FraudIndex.fraud_count(vec14) → Integer 0..5
static VALUE rb_fraud_index_fraud_count(VALUE self, VALUE arr) {
  (void)self;
  Check_Type(arr, T_ARRAY);
  if (!g_loaded) rb_raise(rb_eRuntimeError, "FraudIndex: index not loaded");
  if (RARRAY_LEN(arr) != static_cast<long>(DIM))
    rb_raise(rb_eArgError, "FraudIndex.fraud_count: need %u floats, got %ld", DIM, RARRAY_LEN(arr));
  float q[DIM];
  for (uint32_t i = 0; i < DIM; ++i)
    q[i] = static_cast<float>(NUM2DBL(rb_ary_entry(arr, i)));
  return INT2FIX(fraud_count(g_index, q, g_nprobe));
}

// FraudIndex.fraud_count_payload(body) → Integer 0..5
// Combines parse + score in a single C call. Body is the raw request bytes.
static VALUE rb_fraud_index_fraud_count_payload(VALUE self, VALUE body) {
  (void)self;
  Check_Type(body, T_STRING);
  if (!g_loaded) rb_raise(rb_eRuntimeError, "FraudIndex: index not loaded");
  float q[DIM];
  parse_payload(RSTRING_PTR(body), static_cast<size_t>(RSTRING_LEN(body)), q);
  return INT2FIX(fraud_count(g_index, q, g_nprobe));
}

// FraudIndex.parse_payload(body, out_array) — for parity testing only.
static VALUE rb_fraud_index_parse_payload(VALUE self, VALUE body, VALUE out) {
  (void)self;
  Check_Type(body, T_STRING);
  Check_Type(out, T_ARRAY);
  if (RARRAY_LEN(out) != static_cast<long>(DIM))
    rb_raise(rb_eArgError, "parse_payload: out needs %u slots", DIM);
  float q[DIM];
  parse_payload(RSTRING_PTR(body), static_cast<size_t>(RSTRING_LEN(body)), q);
  for (uint32_t i = 0; i < DIM; ++i)
    rb_ary_store(out, i, DBL2NUM(q[i]));
  return out;
}

static VALUE rb_fraud_index_loaded(VALUE self) {
  (void)self;
  return g_loaded ? Qtrue : Qfalse;
}

static VALUE rb_fraud_index_get_nprobe(VALUE self) {
  (void)self;
  return INT2NUM(g_nprobe);
}

static VALUE rb_fraud_index_set_nprobe(VALUE self, VALUE n) {
  (void)self;
  int v = NUM2INT(n);
  if (v < 1) rb_raise(rb_eArgError, "nprobe must be >= 1");
  g_nprobe = v;
  return INT2NUM(v);
}

void Init_fraud_index(void) {
  VALUE mod = rb_define_module("FraudIndex");
  rb_define_singleton_method(mod, "load",                 RUBY_METHOD_FUNC(rb_fraud_index_load),                  1);
  rb_define_singleton_method(mod, "score",                RUBY_METHOD_FUNC(rb_fraud_index_score),                 1);
  rb_define_singleton_method(mod, "fraud_count",          RUBY_METHOD_FUNC(rb_fraud_index_fraud_count),           1);
  rb_define_singleton_method(mod, "fraud_count_payload",  RUBY_METHOD_FUNC(rb_fraud_index_fraud_count_payload),   1);
  rb_define_singleton_method(mod, "parse_payload",        RUBY_METHOD_FUNC(rb_fraud_index_parse_payload),         2);
  rb_define_singleton_method(mod, "loaded?",              RUBY_METHOD_FUNC(rb_fraud_index_loaded),                0);
  rb_define_singleton_method(mod, "nprobe",               RUBY_METHOD_FUNC(rb_fraud_index_get_nprobe),            0);
  rb_define_singleton_method(mod, "nprobe=",              RUBY_METHOD_FUNC(rb_fraud_index_set_nprobe),            1);
}

} // extern "C"
