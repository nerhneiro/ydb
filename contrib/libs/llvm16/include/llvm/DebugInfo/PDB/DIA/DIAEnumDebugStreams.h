#pragma once

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

//==- DIAEnumDebugStreams.h - DIA Debug Stream Enumerator impl ---*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_DIA_DIAENUMDEBUGSTREAMS_H
#define LLVM_DEBUGINFO_PDB_DIA_DIAENUMDEBUGSTREAMS_H

#include "DIASupport.h"
#include "llvm/DebugInfo/PDB/IPDBDataStream.h"
#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"

namespace llvm {
namespace pdb {

class IPDBDataStream;

class DIAEnumDebugStreams : public IPDBEnumChildren<IPDBDataStream> {
public:
  explicit DIAEnumDebugStreams(CComPtr<IDiaEnumDebugStreams> DiaEnumerator);

  uint32_t getChildCount() const override;
  ChildTypePtr getChildAtIndex(uint32_t Index) const override;
  ChildTypePtr getNext() override;
  void reset() override;

private:
  CComPtr<IDiaEnumDebugStreams> Enumerator;
};
}
}

#endif

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
