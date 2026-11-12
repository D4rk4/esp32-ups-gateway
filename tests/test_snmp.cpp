#include "snmp.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #x); } while (false)

// Independent fixture encoder and decoder. Expected response fields are decoded
// here, never built or parsed through the production implementation.
Bytes tlv(uint8_t tag, Bytes body) {
  Bytes result{tag};
  if (body.size() < 128) result.push_back(uint8_t(body.size()));
  else {
    result.push_back(0x82);
    result.push_back(uint8_t(body.size() >> 8));
    result.push_back(uint8_t(body.size()));
  }
  result.insert(result.end(), body.begin(), body.end());
  return result;
}
void append(Bytes& target, const Bytes& source) {
  target.insert(target.end(), source.begin(), source.end());
}
Bytes si(int32_t value) {
  const uint32_t u = uint32_t(value);
  Bytes bytes{uint8_t(u >> 24), uint8_t(u >> 16), uint8_t(u >> 8), uint8_t(u)};
  while (bytes.size() > 1 &&
         ((bytes[0] == 0 && !(bytes[1] & 128)) ||
          (bytes[0] == 255 && (bytes[1] & 128)))) bytes.erase(bytes.begin());
  return tlv(2, bytes);
}
Bytes obj(const snmp::Oid& oid) {
  Bytes body;
  for (size_t i = 1; i < oid.size(); ++i) {
    uint64_t number = i == 1 ? uint64_t(oid[0]) * 40 + oid[1] : oid[i];
    Bytes digits{uint8_t(number & 127)};
    while ((number >>= 7) != 0) digits.insert(digits.begin(), uint8_t(128 | (number & 127)));
    append(body, digits);
  }
  return tlv(6, body);
}
Bytes bind(const snmp::Oid& oid, Bytes value = {5, 0}) {
  Bytes body = obj(oid);
  append(body, value);
  return tlv(0x30, body);
}
Bytes request(uint8_t pdu, int32_t id, int32_t first, int32_t second,
              const std::vector<Bytes>& bindings, const std::string& community = "public",
              int version = 1) {
  Bytes list;
  for (const auto& binding : bindings) append(list, binding);
  Bytes body = si(id);
  append(body, si(first));
  append(body, si(second));
  append(body, tlv(0x30, list));
  Bytes message = si(version);
  append(message, tlv(4, Bytes(community.begin(), community.end())));
  append(message, tlv(pdu, body));
  return tlv(0x30, message);
}
struct Item { uint8_t tag; Bytes b; };
struct Reader {
  Bytes b;
  size_t p = 0;
  Item read() {
    CHECK(p + 2 <= b.size());
    const uint8_t tag = b[p++];
    size_t length = b[p++];
    if (length & 128) {
      size_t count = length & 127;
      length = 0;
      CHECK(count && p + count <= b.size());
      while (count--) length = length * 256 + b[p++];
    }
    CHECK(length <= b.size() - p);
    Bytes body(b.begin() + p, b.begin() + p + length);
    p += length;
    return {tag, body};
  }
  bool end() const { return p == b.size(); }
};
int32_t integer(const Item& item) {
  CHECK(item.tag == 2 && item.b.size() > 0 && item.b.size() <= 4);
  uint32_t value = (item.b[0] & 128) ? 0xffffffffu : 0;
  for (auto byte : item.b) value = (value << 8) | byte;
  return (value & 0x80000000u) ? int32_t(int64_t(value) - 0x100000000LL) : int32_t(value);
}
snmp::Oid decodeOid(const Item& item) {
  CHECK(item.tag == 6);
  snmp::Oid oid;
  uint64_t value = 0;
  for (auto byte : item.b) {
    value = (value << 7) | (byte & 127);
    if (!(byte & 128)) {
      if (oid.empty()) {
        oid.push_back(value < 40 ? 0 : value < 80 ? 1 : 2);
        oid.push_back(uint32_t(value - oid[0] * 40));
      } else oid.push_back(uint32_t(value));
      value = 0;
    }
  }
  return oid;
}
struct Result {
  int32_t id, status, index;
  uint8_t pdu;
  std::vector<std::pair<snmp::Oid, Item>> binds;
};
Result decode(const Bytes& bytes) {
  Reader top{bytes};
  auto outer = top.read();
  CHECK(outer.tag == 0x30 && top.end());
  Reader message{outer.b};
  CHECK(integer(message.read()) == 1);
  CHECK(message.read().tag == 4);
  auto pdu = message.read();
  CHECK(message.end());
  Reader fields{pdu.b};
  Result result{};
  result.pdu = pdu.tag;
  result.id = integer(fields.read());
  result.status = integer(fields.read());
  result.index = integer(fields.read());
  auto list = fields.read();
  CHECK(fields.end() && list.tag == 0x30);
  Reader bindings{list.b};
  while (!bindings.end()) {
    auto binding = bindings.read();
    CHECK(binding.tag == 0x30);
    Reader parts{binding.b};
    auto oid = decodeOid(parts.read());
    auto value = parts.read();
    CHECK(parts.end());
    result.binds.push_back({oid, value});
  }
  return result;
}
const snmp::Oid O1{1,3,6,1,2,1,1,1,0}, O2{1,3,6,1,2,1,1,2,0}, O3{1,3,6,1,2,1,1,3,0};
struct Mib : snmp::Provider {
  std::vector<snmp::VarBind> rows;
  bool broken = false;
  bool get(const snmp::Oid& oid, snmp::Value& value) const override {
    for (const auto& row : rows) if (row.oid == oid) {
      value = row.value;
      return true;
    }
    return false;
  }
  bool next(const snmp::Oid& oid, snmp::Oid& name, snmp::Value& value) const override {
    if (broken) { name = oid; return true; }
    for (const auto& row : rows) if (oid < row.oid) {
      name = row.oid;
      value = row.value;
      return true;
    }
    return false;
  }
};
Mib mib() {
  snmp::Value description;
  description.type = snmp::Type::OctetString;
  description.text = "UPS";
  snmp::Value object;
  object.type = snmp::Type::ObjectId;
  object.oid = {2,999,4294967295u};
  snmp::Value ticks;
  ticks.type = snmp::Type::TimeTicks;
  ticks.unsigned_value = 0xffffffff;
  Mib result;
  result.rows = {{O1,description}, {O2,object}, {O3,ticks}};
  return result;
}
Bytes run(const Bytes& request, const Mib& provider = mib()) {
  Bytes output{1,2};
  CHECK(snmp::processRequest(request.data(), request.size(), "public", provider, output));
  CHECK(output.size() <= 1400);
  return output;
}

void oid_text() {
  snmp::Oid o;
  CHECK(snmp::parseOid(".2.999.4294967295",o));
  CHECK((o==snmp::Oid{2,999,4294967295u}));
  CHECK(snmp::oidString(o)=="2.999.4294967295");
  for(auto s:{"","1","3.0","1.40","1..3","1.3.","1.-3","1.3a","1.4294967296"," 1.3"}) {
    CHECK(!snmp::parseOid(s,o));
    CHECK(o.empty());
  }std::string big="1.3";
  for(int i=0;i<31;i++)big+=".1";
  CHECK(!snmp::parseOid(big,o));
}
void get_bytes() {
  auto q=request(0xa0,-2147483647,0,0,{bind(O1),bind(O3),bind(O2)});
  auto r=decode(run(q));
  CHECK(r.pdu==0xa2&&r.id==-2147483647&&r.status==0&&r.index==0);
  CHECK(r.binds.size()==3);
  CHECK(r.binds[0].second.tag==4&&r.binds[0].second.b==Bytes({'U','P','S'}));
  CHECK(r.binds[1].second.tag==0x43&&r.binds[1].second.b==Bytes({0,255,255,255,255}));
  CHECK((decodeOid(r.binds[2].second)==snmp::Oid{2,999,4294967295u}));
}
void get_missing() {
  auto r=decode(run(request(0xa0,7,0,0,{bind({1,3,6,99})})));
  CHECK(r.binds.size()==1&&r.binds[0].second.tag==0x81&&r.binds[0].second.b.empty());
}
void next_order() {
  auto r=decode(run(request(0xa1,3,0,0,{bind({1,3}),bind(O1),bind(O3)})));
  CHECK(r.binds.size()==3&&r.binds[0].first==O1&&r.binds[1].first==O2&&r.binds[2].first==O3);
  CHECK(r.binds[2].second.tag==0x82);
  Mib m=mib();
  m.broken=true;
  auto z=decode(run(request(0xa1,1,0,0,{bind(O1)}),m));
  CHECK(z.binds[0].first==O1&&z.binds[0].second.tag==0x82);
}
void bulk_columns() {
  auto r=decode(run(request(0xa5,9,1,5,{bind({1,3}),bind(O1),bind(O2)})));
  CHECK(r.binds.size()==7);
  CHECK(r.binds[0].first==O1);
  CHECK(r.binds[1].first==O2&&r.binds[2].first==O3);
  CHECK(r.binds[3].first==O3&&r.binds[4].first==O3&&r.binds[4].second.tag==0x82);
  CHECK(r.binds[5].second.tag==0x82&&r.binds[6].second.tag==0x82);
  auto neg=decode(run(request(0xa5,1,-2,-3,{bind(O1)})));
  CHECK(neg.binds.empty());
  auto capped=decode(run(request(0xa5,1,0,2147483647,{bind({1,3})})));
  CHECK(capped.binds.size()==4);
}
void set_rejected() {
  auto q=request(0xa3,-1,0,0,{bind(O1,tlv(4,{'n','e','w'})),bind(O3,tlv(0x43,{0,255,255,255,255}))});
  auto r=decode(run(q));
  CHECK(r.id==-1&&r.status==17&&r.index==1&&r.binds.size()==2);
  CHECK(r.binds[0].second.b==Bytes({'n','e','w'}));
  auto ip=decode(run(request(0xa3,1,0,0,{bind(O1,tlv(0x40,{127,0,0,1}))})));
  CHECK(ip.status==17&&ip.binds[0].second.tag==0x40);
}
void malformed() {
  auto good=request(0xa0,1,0,0,{bind(O1)});
  std::vector<Bytes> bad{{},{0x30,0x80,0,0},{0x30,0xff},{0x30,0x89,255,255,255,255,255,255,255,255,255},request(0xa0,1,0,0,{bind(O1)},"private"),request(0xa0,1,0,0,{bind(O1)},"public",0),request(0xa4,1,0,0,{bind(O1)})};
  auto trailing=good;
  trailing.push_back(0);
  bad.push_back(trailing);
  for(size_t i=0;i<good.size();i++)bad.emplace_back(good.begin(),good.begin()+i);
  bad.push_back(request(0xa0,1,0,0,{tlv(0x30,{6,1,128,5,0})}));
  bad.push_back(request(0xa0,1,0,0,{tlv(0x30,{6,2,128,0,5,0})}));
  bad.push_back(request(0xa0,1,0,0,{bind(O1,tlv(0x43,{1,0,0,0,0}))}));
  bad.push_back(request(0xa0,1,0,0,{bind(O1,tlv(0x43,{255}))}));
  bad.push_back(request(0xa0,1,0,0,{bind(O1,{5,1,0})}));
  std::vector<Bytes> many(33,bind(O1));
  bad.push_back(request(0xa0,1,0,0,many));
  bad.push_back(request(0xa0,1,0,0,{bind(O1)},std::string(65,'x')));
  snmp::Oid longoid{1,3};
  for(int i=0;i<31;i++)longoid.push_back(1);
  bad.push_back(request(0xa0,1,0,0,{bind(longoid)}));
  bad.push_back(Bytes(1401));
  for(auto&q:bad) {
    Bytes out{1};
    CHECK(!snmp::processRequest(q.data(),q.size(),"public",mib(),out));
    CHECK(out.empty());
  }Bytes out{1};
  CHECK(!snmp::processRequest(nullptr,10,"public",mib(),out)&&out.empty());
}
void too_big() {
  Mib m=mib();
  m.rows[0].value.text=std::string(1000,'x');
  auto r=decode(run(request(0xa0,1,0,0,{bind(O1),bind(O1)}),m));
  CHECK(r.status==1&&r.index==0&&r.binds.empty());
  auto next = decode(run(request(0xa1,2,0,0,{bind({1,3}),bind({1,3})}),m));
  CHECK(next.status == 1 && next.index == 0 && next.binds.empty());
}
void trap() {
  snmp::Oid trapOid{1,3,6,1,6,3,1,1,5,1};
  Bytes out;
  CHECK(snmp::encodeTrap("public",0xffffffff,trapOid,{{O1,mib().rows[0].value}},out));
  auto r=decode(out);
  CHECK(r.pdu==0xa7&&r.id!=0&&r.status==0&&r.index==0&&r.binds.size()==3);
  CHECK((r.binds[0].first==snmp::Oid{1,3,6,1,2,1,1,3,0}));
  CHECK(r.binds[0].second.tag==0x43&&r.binds[0].second.b==Bytes({0,255,255,255,255}));
  CHECK((r.binds[1].first==snmp::Oid{1,3,6,1,6,3,1,1,4,1,0}));
  CHECK(decodeOid(r.binds[1].second)==trapOid);
  CHECK(r.binds[2].first==O1);
  CHECK(!snmp::encodeTrap(std::string(65,'x'),0,trapOid,{},out)&&out.empty());
  CHECK(!snmp::encodeTrap("public",0,{1,40},{},out)&&out.empty());
  std::vector<snmp::VarBind> extras(63,{O1,{}});
  CHECK(!snmp::encodeTrap("public",0,trapOid,extras,out)&&out.empty());
}

void oid_wire_boundaries() {
  snmp::Oid oid{2, 4294967295u};
  for (int i=0;i<30;++i) oid.push_back(4294967295u);
  snmp::Value v; v.type=snmp::Type::ObjectId; v.oid=oid;
  Mib m; m.rows={{oid,v}};
  auto r=decode(run(request(0xa0,INT32_MIN,0,0,{bind(oid)}),m));
  CHECK(r.id==INT32_MIN && r.binds[0].first==oid);
  CHECK(decodeOid(r.binds[0].second)==oid);
  Bytes out;
  // The first combined subidentifier is UINT32_MAX+81: one too large.
  auto invalidFirst=tlv(0x30,{6,5,0x90,0x80,0x80,0x80,0x50,5,0});
  auto q=request(0xa0,0,0,0,{invalidFirst});
  CHECK(!snmp::processRequest(q.data(),q.size(),"public",m,out));
  // A later subidentifier is UINT32_MAX+1.
  auto invalidArc=tlv(0x30,{6,6,43,0x90,0x80,0x80,0x80,0x00,5,0});
  q=request(0xa0,0,0,0,{invalidArc});
  CHECK(!snmp::processRequest(q.data(),q.size(),"public",m,out));
}
struct Endless: snmp::Provider {
  mutable size_t calls=0;
  bool get(const snmp::Oid&,snmp::Value&)const override{return false;}
  bool next(const snmp::Oid& o,snmp::Oid& n,snmp::Value& v)const override {
    ++calls; n=o; ++n.back(); v={}; return true;
  }
};
void bulk_bound() {
  Endless provider; Bytes out;
  auto q=request(0xa5,10,0,INT32_MAX,{bind({0,0}),bind({0,1})});
  CHECK(snmp::processRequest(q.data(),q.size(),"public",provider,out));
  auto r=decode(out); CHECK(r.status==0 && r.binds.size()==64 && provider.calls==64);
  CHECK((r.binds[62].first==snmp::Oid{0,32}));
  CHECK((r.binds[63].first==snmp::Oid{0,33}));
  // Nonrepeaters are clamped to the number of supplied variables.
  auto nr=decode(run(request(0xa5,11,INT32_MAX,INT32_MAX,{bind(O1),bind(O2)})));
  CHECK(nr.binds.size()==2 && nr.binds[0].first==O2 && nr.binds[1].first==O3);
}

void bulk_size_limited_prefix() {
  Mib provider;
  std::vector<snmp::Oid> roots;
  // Independent three-column MIB: 25 requested rows exceed both the 1400-byte
  // wire budget and the 64-binding count budget. Every value is real data.
  for (uint32_t column = 1; column <= 3; ++column) {
    snmp::Oid root{1,3,6,1,4,1,4242,column};
    roots.push_back(root);
    for (uint32_t row = 1; row <= 40; ++row) {
      auto name = root;
      name.push_back(row);
      snmp::Value value;
      value.type = snmp::Type::OctetString;
      value.text = std::string(100, char('A' + column - 1));
      provider.rows.push_back({name,value});
    }
  }
  const auto q = request(0xa5,123,0,25,{bind(roots[0]),bind(roots[1]),bind(roots[2])});
  const auto response = run(q, provider);
  const auto result = decode(response);
  CHECK(result.id == 123 && result.status == 0 && result.index == 0);
  CHECK(!result.binds.empty() && result.binds.size() < 64);

  // Independently calculate the canonical envelope size, including transitions
  // in all four length fields. Varbind fixtures here each use short lengths.
  const auto tlvSize = [](size_t body) {
    return body + (body < 128 ? 2u : body <= 255 ? 3u : 4u);
  };
  const auto responseSize = [&](size_t bindingsBytes) {
    return tlvSize(3 + 8 + tlvSize(9 + tlvSize(bindingsBytes)));
  };
  size_t bytes = 0;
  auto cursors = roots;
  for (size_t i = 0; i < result.binds.size(); ++i) {
    const size_t column = i % roots.size();
    auto expected = roots[column];
    expected.push_back(uint32_t(i / roots.size()) + 1);
    CHECK(result.binds[i].first == expected);
    CHECK(result.binds[i].second.tag == 4);
    CHECK(result.binds[i].second.b == Bytes(100, uint8_t('A' + column)));
    CHECK(cursors[column] < expected);
    cursors[column] = expected;
    bytes += bind(expected,tlv(4,Bytes(100,uint8_t('A' + column)))).size();
  }
  CHECK(response.size() == responseSize(bytes) && response.size() <= 1400);
  const size_t nextColumn = result.binds.size() % roots.size();
  auto nextName = roots[nextColumn];
  nextName.push_back(uint32_t(result.binds.size() / roots.size()) + 1);
  const size_t nextBytes = bind(nextName,tlv(4,Bytes(100,uint8_t('A' + nextColumn)))).size();
  CHECK(responseSize(bytes + nextBytes) > 1400); // No additional binding fits.
  CHECK(result.binds.size() % roots.size() != 0); // Truncation can split a row.

  // A manager advances each column to its last returned name. The next packet
  // must continue all three columns even when the prior packet ended mid-row.
  const auto followup = decode(run(request(0xa5,124,0,25,
      {bind(cursors[0]),bind(cursors[1]),bind(cursors[2])}), provider));
  CHECK(followup.status == 0 && followup.binds.size() >= roots.size());
  for (size_t column = 0; column < roots.size(); ++column) {
    auto expected = cursors[column];
    ++expected.back();
    CHECK(followup.binds[column].first == expected);
  }

  // A single oversized BULK value still gives a valid empty noError prefix.
  provider.rows[0].value.text = std::string(1400, 'x');
  const auto empty = decode(run(request(0xa5,125,0,25,{bind(roots[0])}), provider));
  CHECK(empty.status == 0 && empty.index == 0 && empty.binds.empty());
}

void strict_integer_encoding() {
  for (const auto& value:std::vector<Bytes>{tlv(2,{0,0}),tlv(2,{255,255}),tlv(0x43,{0,1}),tlv(0x46,{0,1})}) {
    auto q=request(0xa3,1,0,0,{bind(O1,value)}); Bytes out;
    CHECK(!snmp::processRequest(q.data(),q.size(),"public",mib(),out));
    CHECK(out.empty());
  }
}
void trap_limits() {
  Bytes out{1}; snmp::Value huge; huge.type=snmp::Type::OctetString; huge.text=std::string(1390,'x');
  CHECK(!snmp::encodeTrap("public",0,O1,{{O2,huge}},out)&&out.empty());
  snmp::Value v;v.type=snmp::Type::Integer;v.integer=INT32_MIN;
  CHECK(snmp::encodeTrap("public",0,O1,{{O2,v}},out));
  auto r=decode(out); CHECK(integer(r.binds[2].second)==INT32_MIN);
}


void exact_get_response() {
  // Hand-written canonical BER response for request-id 7 and sysDescr.0="UPS".
  const Bytes expected{
    0x30,0x29, 0x02,0x01,0x01, 0x04,0x06,'p','u','b','l','i','c',
    0xa2,0x1c, 0x02,0x01,0x07, 0x02,0x01,0x00, 0x02,0x01,0x00,
    0x30,0x11, 0x30,0x0f, 0x06,0x08,0x2b,0x06,0x01,0x02,0x01,0x01,0x01,0x00,
    0x04,0x03,'U','P','S'
  };
  CHECK(run(request(0xa0,7,0,0,{bind(O1)}))==expected);
}
void mutation_corpus() {
  const auto original=request(0xa5,-19,1,3,{bind(O1),bind(O2)});
  Mib provider=mib();
  for(size_t position=0;position<original.size();++position) {
    for(unsigned byte=0;byte<256;++byte) {
      auto q=original;q[position]=uint8_t(byte);Bytes out{255};
      if(snmp::processRequest(q.data(),q.size(),"public",provider,out)) {
        CHECK(out.size()<=1400);CHECK(decode(out).pdu==0xa2);
      } else CHECK(out.empty());
    }
  }
  uint32_t random=0xc0ffee;
  for(size_t trial=0;trial<3000;++trial) {
    random=random*1664525u+1013904223u;Bytes q(random%1402);
    for(auto& byte:q) {random=random*1664525u+1013904223u;byte=uint8_t(random>>24);}
    Bytes out{255};
    if(snmp::processRequest(q.data(),q.size(),"public",provider,out)) {
      CHECK(out.size()<=1400);CHECK(decode(out).pdu==0xa2);
    } else CHECK(out.empty());
  }
}

int main() {
  int fails=0;
  for(auto test:{oid_text,get_bytes,get_missing,next_order,bulk_columns,set_rejected,malformed,too_big,trap,oid_wire_boundaries,bulk_bound,bulk_size_limited_prefix,strict_integer_encoding,trap_limits,exact_get_response,mutation_corpus}) {
    try{test();
    }catch(const std::exception&e) {
      std::fprintf(stderr,"FAIL %s\n",e.what());
      ++fails;
    }}if(fails)return 1;
  std::puts("SNMP tests: 16 groups passed");
}
