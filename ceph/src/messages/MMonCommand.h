// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MMONCOMMAND_H
#define CEPH_MMONCOMMAND_H

#include "messages/PaxosServiceMessage.h"

#include <vector>
#include <string>

using TOPNSPC::common::cmdmap_from_json;
using TOPNSPC::common::cmd_getval;

class MMonCommand : public PaxosServiceMessage {
public:
  // weird note: prior to octopus, MgrClient would leave fsid blank when
  // sending commands to the mgr.  Starting with octopus, this is either
  // populated with a valid fsid (tell command) or an MMgrCommand is sent
  // instead.
  uuid_d fsid;
  std::vector<std::string> cmd;

  MMonCommand() : PaxosServiceMessage{MSG_MON_COMMAND, 0} {}
  MMonCommand(const uuid_d &f)
    : PaxosServiceMessage{MSG_MON_COMMAND, 0},
      fsid(f)
  { }

private:
  ~MMonCommand() override {}

public:
  std::string_view get_type_name() const override { return "mon_command"; }
  void print(std::ostream& o) const override {
    cmdmap_t cmdmap;
    stringstream ss;
    string prefix;
    cmdmap_from_json(cmd, &cmdmap, ss);
    cmd_getval(cmdmap, "prefix", prefix);
    // Some config values contain sensitive data, so don't log them
    o << "mon_command(";
    if (prefix == "config set") {
      string name;
      cmd_getval(cmdmap, "name", name);
      o << "[{prefix=" << prefix << ", name=" << name << "}]";
    } else if (prefix == "config-key set") {
      string key;
      cmd_getval(cmdmap, "key", key);
      o << "[{prefix=" << prefix << ", key=" << key << "}]";
    } else {
      for (unsigned i=0; i<cmd.size(); i++) {
        if (i) o << ' ';
        o << cmd[i];
      }
    }
    o << " v " << version << ")";
  }

  void encode_payload(uint64_t features) override {
    using ceph::encode;
    paxos_encode();
    encode(fsid, payload);
    encode(cmd, payload);
  }
  void decode_payload() override {
    using ceph::decode;
    auto p = payload.cbegin();
    paxos_decode(p);
    decode(fsid, p);
    decode(cmd, p);
  }
private:
  template<class T, typename... Args>
  friend boost::intrusive_ptr<T> ceph::make_message(Args&&... args);
};

#endif
