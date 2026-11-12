#include "snmp.hpp"
#include <algorithm>
#include <limits>

// Protocol operations: https://www.rfc-editor.org/rfc/rfc3416.html#section-4.2
// BER message envelope: https://www.rfc-editor.org/rfc/rfc1157.html#section-4
// Only SNMPv2c is implemented. All wire buffers, lists and OIDs are capped below;
// parsing uses fixed-depth readers rather than recursive ASN.1 decoding.
namespace snmp {
namespace {
constexpr size_t kPacket = 1400, kIncoming = 32, kReturned = 64;
constexpr size_t kArcs = 32, kCommunity = 64;
using Bytes = std::vector<uint8_t>;

struct Slice { const uint8_t* data = nullptr; size_t size = 0; };
struct Element { uint8_t tag = 0; Slice body, wire; };
class Reader {
 public:
  explicit Reader(Slice s) : s_(s) {}
  bool done() const { return pos_ == s_.size; }
  bool read(Element& e) {
    const size_t start = pos_;
    if (s_.size - pos_ < 2) return false;
    e.tag = s_.data[pos_++];
    // SMI uses only one-octet tags. Reject high-tag-number ASN.1 forms.
    if ((e.tag & 0x1f) == 0x1f) return false;
    size_t length = s_.data[pos_++];
    if (length & 0x80) {
      const size_t count = length & 0x7f;
      if (count == 0 || count > sizeof(size_t) || count > s_.size - pos_) return false;
      length = 0;
      for (size_t i = 0; i < count; ++i) {
        if (length > (std::numeric_limits<size_t>::max() >> 8)) return false;
        length = (length << 8) | s_.data[pos_++];
      }
    }
    if (length > s_.size - pos_) return false;
    e.body = {s_.data + pos_, length};
    pos_ += length;
    e.wire = {s_.data + start, pos_ - start};
    return true;
  }
  bool read(uint8_t tag, Element& e) { return read(e) && e.tag == tag; }
 private:
  Slice s_;
  size_t pos_ = 0;
};

bool validOid(const Oid& oid) {
  return oid.size() >= 2 && oid.size() <= kArcs && oid[0] <= 2 &&
         (oid[0] == 2 || oid[1] <= 39);
}
bool decodeSigned(const Element& e, int32_t& value) {
  if (e.tag != 2 || e.body.size == 0 || e.body.size > 4) return false;
  if (e.body.size > 1 &&
      ((e.body.data[0] == 0 && !(e.body.data[1] & 0x80)) ||
       (e.body.data[0] == 0xff && (e.body.data[1] & 0x80)))) return false;
  uint32_t v = (e.body.data[0] & 0x80) ? 0xffffffffu : 0;
  for (size_t i = 0; i < e.body.size; ++i) v = (v << 8) | e.body.data[i];
  // Conversion via int64_t avoids implementation-defined unsigned-to-signed overflow.
  value = (v & 0x80000000u) ? int32_t(int64_t(v) - 0x100000000LL) : int32_t(v);
  return true;
}
bool signedField(Reader& reader, int32_t& value) {
  Element e;
  return reader.read(2, e) && decodeSigned(e, value);
}
bool validUnsigned(Slice s, size_t width) {
  if (s.size == 0 || s.size > width + 1 || (s.data[0] & 0x80)) return false;
  if (s.size > 1 && s.data[0] == 0 && !(s.data[1] & 0x80)) return false;
  // A sign octet may extend the width, but must not extend the magnitude.
  return s.size <= width || s.data[0] == 0;
}
bool decodeOid(Slice s, Oid& out) {
  out.clear();
  if (s.size == 0) return false;
  size_t pos = 0;
  while (pos < s.size) {
    if (out.size() >= kArcs || s.data[pos] == 0x80) return false;
    const uint64_t maximum = out.empty() ? uint64_t(UINT32_MAX) + 80 : UINT32_MAX;
    uint64_t value = 0;
    uint8_t octet;
    do {
      if (pos == s.size) return false;
      octet = s.data[pos++];
      if (value > (maximum >> 7)) return false;
      value = (value << 7) | (octet & 0x7f);
      if (value > maximum) return false;
    } while (octet & 0x80);
    if (out.empty()) {
      const uint32_t first = value < 40 ? 0 : value < 80 ? 1 : 2;
      out.push_back(first);
      out.push_back(uint32_t(value - first * 40));
    } else out.push_back(uint32_t(value));
  }
  return validOid(out);
}
bool validValue(const Element& e) {
  int32_t ignored = 0;
  switch (e.tag) {
    case 0x02: return decodeSigned(e, ignored);
    case 0x04: case 0x44: return e.body.size <= kPacket; // OCTET STRING / Opaque
    case 0x05: case 0x80: case 0x81: case 0x82: return e.body.size == 0;
    case 0x06: { Oid oid; return decodeOid(e.body, oid); }
    case 0x40: return e.body.size == 4; // IpAddress, echoed by rejected SET
    case 0x41: case 0x42: case 0x43: return validUnsigned(e.body, 4);
    case 0x46: return validUnsigned(e.body, 8); // Counter64, echoed by rejected SET
    default: return false;
  }
}
struct InputBind { Oid oid; Slice wire; };
struct Request {
  uint8_t pdu = 0;
  int32_t id = 0, first = 0, second = 0;
  std::vector<InputBind> bindings;
};
bool parse(const uint8_t* data, size_t size, const std::string& community, Request& req) {
  if (!data || size == 0 || size > kPacket || community.size() > kCommunity) return false;
  Reader root({data, size}); Element outer;
  if (!root.read(0x30, outer) || !root.done()) return false;
  Reader message(outer.body); int32_t version;
  Element comm, pdu;
  if (!signedField(message, version) || version != 1 || !message.read(4, comm) ||
      comm.body.size > kCommunity || comm.body.size != community.size() ||
      !std::equal(community.begin(), community.end(), comm.body.data,
                  [](char a, uint8_t b) { return uint8_t(a) == b; }) ||
      !message.read(pdu) || !message.done()) return false;
  if (pdu.tag != 0xa0 && pdu.tag != 0xa1 && pdu.tag != 0xa3 && pdu.tag != 0xa5) return false;
  req.pdu = pdu.tag;
  Reader body(pdu.body); Element bindings;
  if (!signedField(body, req.id) || !signedField(body, req.first) ||
      !signedField(body, req.second) || !body.read(0x30, bindings) || !body.done()) return false;
  Reader list(bindings.body);
  while (!list.done()) {
    if (req.bindings.size() == kIncoming) return false;
    Element binding, oid, value;
    if (!list.read(0x30, binding)) return false;
    Reader fields(binding.body);
    InputBind item;
    if (!fields.read(6, oid) || !decodeOid(oid.body, item.oid) ||
        !fields.read(value) || !validValue(value) || !fields.done()) return false;
    item.wire = binding.wire;
    req.bindings.push_back(std::move(item));
  }
  return true;
}

// Every append checks the byte cap before allocation. Containers grow only to
// protocol limits; encoder copies validate the provider strings/OIDs first.
bool append(Bytes& target, const uint8_t* source, size_t count) {
  if (count > kPacket - target.size()) return false;
  if (count) target.insert(target.end(), source, source + count);
  return true;
}
bool append(Bytes& target, const Bytes& source) { return append(target, source.data(), source.size()); }
bool byte(Bytes& target, uint8_t value) { return append(target, &value, 1); }
size_t wrappedSize(size_t bodySize) {
  return bodySize + (bodySize < 128 ? 2 : bodySize <= 255 ? 3 : 4);
}
bool wrapped(Bytes& target, uint8_t tag, const Bytes& body) {
  if (wrappedSize(body.size()) > kPacket - target.size()) return false;
  byte(target, tag);
  if (body.size() < 128) byte(target, uint8_t(body.size()));
  else if (body.size() <= 255) { byte(target, 0x81); byte(target, uint8_t(body.size())); }
  else { byte(target, 0x82); byte(target, uint8_t(body.size() >> 8)); byte(target, uint8_t(body.size())); }
  return append(target, body);
}
bool signedInteger(Bytes& target, int32_t value) {
  const uint32_t v = uint32_t(value);
  uint8_t data[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  size_t start = 0;
  while (start < 3 && ((data[start] == 0 && !(data[start + 1] & 0x80)) ||
                       (data[start] == 0xff && (data[start + 1] & 0x80)))) ++start;
  Bytes body(data + start, data + 4);
  return wrapped(target, 2, body);
}
bool unsignedInteger(Bytes& target, uint8_t tag, uint32_t value) {
  uint8_t data[5] = {0, uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
  size_t start = 0;
  while (start < 4 && data[start] == 0 && !(data[start + 1] & 0x80)) ++start;
  return wrapped(target, tag, Bytes(data + start, data + 5));
}
bool subidentifier(Bytes& target, uint64_t value) {
  uint8_t bytes[5]; size_t count = 0;
  do { bytes[count++] = uint8_t(value & 0x7f); value >>= 7; } while (value);
  while (count) { --count; if (!byte(target, uint8_t(bytes[count] | (count ? 0x80 : 0)))) return false; }
  return true;
}
bool objectId(Bytes& target, const Oid& oid) {
  if (!validOid(oid)) return false;
  Bytes body;
  if (!subidentifier(body, uint64_t(oid[0]) * 40 + oid[1])) return false;
  for (size_t i = 2; i < oid.size(); ++i) if (!subidentifier(body, oid[i])) return false;
  return wrapped(target, 6, body);
}
bool valueBytes(Bytes& target, const Value& value) {
  const uint8_t tag = uint8_t(value.type);
  switch (value.type) {
    case Type::Integer: return signedInteger(target, value.integer);
    case Type::Counter32: case Type::Gauge32: case Type::TimeTicks:
      return unsignedInteger(target, tag, value.unsigned_value);
    case Type::OctetString: {
      if (value.text.size() > kPacket) return false;
      Bytes body(value.text.begin(), value.text.end());
      return wrapped(target, tag, body);
    }
    case Type::ObjectId: return objectId(target, value.oid);
    case Type::Null: case Type::NoSuchObject: case Type::NoSuchInstance: case Type::EndOfMibView:
      return wrapped(target, tag, {});
    default: return false;
  }
}
bool bindingBytes(Bytes& target, const Oid& oid, const Value& value) {
  Bytes body;
  return objectId(body, oid) && valueBytes(body, value) && wrapped(target, 0x30, body);
}
bool envelope(const std::string& community, uint8_t tag, int32_t id,
              int32_t error, int32_t index, const Bytes& bindings, Bytes& output) {
  Bytes body, pdu, message;
  if (!signedInteger(body, id) || !signedInteger(body, error) || !signedInteger(body, index) ||
      !wrapped(body, 0x30, bindings) || !wrapped(pdu, tag, body) ||
      !signedInteger(message, 1)) return false;
  const Bytes comm(community.begin(), community.end());
  return wrapped(message, 4, comm) && append(message, pdu) && wrapped(output, 0x30, message);
}
Value exception(Type type) { Value value; value.type = type; return value; }
bool nextValue(const Provider& provider, const Oid& from, Oid& name, Value& value) {
  Oid candidate;
  if (!provider.next(from, candidate, value) || !validOid(candidate) || !(from < candidate)) {
    name = from; value = exception(Type::EndOfMibView); return false;
  }
  name = std::move(candidate);
  return true;
}
}  // namespace

bool parseOid(const std::string& text, Oid& output) {
  output.clear();
  // Each of 32 uint32 arcs needs at most ten digits plus a dot.
  if (text.empty() || text.size() > kArcs * 11) return false;
  size_t pos = text[0] == '.' ? 1 : 0;
  Oid parsed;
  while (pos < text.size()) {
    if (parsed.size() == kArcs || text[pos] < '0' || text[pos] > '9') return false;
    uint32_t value = 0;
    do {
      const uint32_t digit = uint32_t(text[pos++] - '0');
      if (value > (UINT32_MAX - digit) / 10) return false;
      value = value * 10 + digit;
    } while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9');
    parsed.push_back(value);
    if (pos < text.size() && (text[pos++] != '.' || pos == text.size())) return false;
  }
  if (!validOid(parsed)) return false;
  output = std::move(parsed);
  return true;
}
std::string oidString(const Oid& oid) {
  if (!validOid(oid)) return {};
  std::string text;
  for (uint32_t arc : oid) { if (!text.empty()) text += '.'; text += std::to_string(arc); }
  return text;
}

bool processRequest(const uint8_t* data, size_t size, const std::string& community,
                    const Provider& provider, std::vector<uint8_t>& output) {
  output.clear();
  Request request;
  if (!parse(data, size, community, request)) return false;
  Bytes bindings;
  bool fits = true;
  int32_t error = 0, errorIndex = 0;
  if (request.pdu == 0xa3) {
    // There is deliberately no mutation API. Echo validated varbind encodings,
    // including valid v2c types not represented by the read-only Value API.
    error = 17; errorIndex = 1;
    for (const auto& item : request.bindings)
      if (!(fits = append(bindings, item.wire.data, item.wire.size))) break;
  } else if (request.pdu != 0xa5) {
    for (const auto& item : request.bindings) {
      Oid name;
      Value value;
      if (request.pdu == 0xa0) {
        name = item.oid;
        if (!provider.get(name, value)) value = exception(Type::NoSuchInstance);
      } else nextValue(provider, item.oid, name, value);
      if (!(fits = bindingBytes(bindings, name, value))) break;
    }
  } else {
    Bytes responseFields;
    signedInteger(responseFields, request.id);
    signedInteger(responseFields, 0);
    signedInteger(responseFields, 0);
    const auto appendBulkBinding = [&](const Oid& name, const Value& value) {
      const size_t previous = bindings.size();
      if (bindingBytes(bindings, name, value)) {
        // Include the varbind-list, PDU, community and message BER length
        // octets, which themselves grow at the 128/256-byte boundaries.
        const size_t pdu = wrappedSize(responseFields.size() + wrappedSize(bindings.size()));
        const size_t message = wrappedSize(3 + wrappedSize(community.size()) + pdu);
        if (message <= kPacket) return true;
      }
      bindings.resize(previous);
      return false;
    };
    const size_t nonRepeaters = std::min(request.bindings.size(), size_t(std::max(int32_t(0), request.first)));
    const uint32_t repetitions = uint32_t(std::max(int32_t(0), request.second));
    size_t returned = 0;
    for (size_t i = 0; i < nonRepeaters; ++i) {
      Oid name; Value value;
      nextValue(provider, request.bindings[i].oid, name, value);
      ++returned;
      if (!(fits = appendBulkBinding(name, value))) break;
    }
    struct Column { Oid name; bool ended = false; };
    std::vector<Column> columns;
    for (size_t i = nonRepeaters; i < request.bindings.size(); ++i)
      columns.push_back({request.bindings[i].oid, false});
    for (uint32_t row = 0; fits && row < repetitions && !columns.empty() && returned < kReturned; ++row) {
      bool allEnded = true;
      for (auto& column : columns) {
        if (returned == kReturned) break;
        Value value;
        Oid name;
        if (column.ended) { name = column.name; value = exception(Type::EndOfMibView); }
        else {
          column.ended = !nextValue(provider, column.name, name, value);
          column.name = name;
        }
        allEnded = allEnded && column.ended;
        ++returned;
        if (!(fits = appendBulkBinding(name, value))) break;
      }
      if (allEnded) break;
    }
  }
  // RFC 3416 section 4.2.3: size-limited GETBULK returns the ordered
  // prefix that fits, possibly ending partway through a row, with noError.
  if (request.pdu == 0xa5)
    return envelope(community, 0xa2, request.id, 0, 0, bindings, output);
  if (fits && envelope(community, 0xa2, request.id, error, errorIndex, bindings, output)) return true;
  // GET/GETNEXT/SET use the distinct tooBig response with an empty list.
  output.clear();
  return envelope(community, 0xa2, request.id, 1, 0, {}, output);
}

bool encodeTrap(const std::string& community, uint32_t uptimeTicks, const Oid& trapOid,
                const std::vector<VarBind>& extra, std::vector<uint8_t>& output) {
  output.clear();
  if (community.size() > kCommunity || !validOid(trapOid) || extra.size() > kReturned - 2) return false;
  Bytes bindings;
  Value uptime; uptime.type = Type::TimeTicks; uptime.unsigned_value = uptimeTicks;
  Value trap; trap.type = Type::ObjectId; trap.oid = trapOid;
  if (!bindingBytes(bindings, {1,3,6,1,2,1,1,3,0}, uptime) ||
      !bindingBytes(bindings, {1,3,6,1,6,3,1,1,4,1,0}, trap)) return false;
  for (const auto& item : extra) if (!bindingBytes(bindings, item.oid, item.value)) return false;
  if (envelope(community, 0xa7, 1, 0, 0, bindings, output)) return true;
  output.clear();
  return false;
}
}  // namespace snmp
