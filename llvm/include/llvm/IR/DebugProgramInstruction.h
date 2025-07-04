//===-- llvm/DebugProgramInstruction.h - Stream of debug info ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Data structures for storing variable assignment information in LLVM. In the
// dbg.value design, a dbg.value intrinsic specifies the position in a block
// a source variable take on an LLVM Value:
//
//    %foo = add i32 1, %0
//    dbg.value(metadata i32 %foo, ...)
//    %bar = void call @ext(%foo);
//
// and all information is stored in the Value / Metadata hierachy defined
// elsewhere in LLVM. In the "DbgRecord" design, each instruction /may/ have a
// connection with a DbgMarker, which identifies a position immediately before
// the instruction, and each DbgMarker /may/ then have connections to DbgRecords
// which record the variable assignment information. To illustrate:
//
//    %foo = add i32 1, %0
//       ; foo->DebugMarker == nullptr
//       ;; There are no variable assignments / debug records "in front" of
//       ;; the instruction for %foo, therefore it has no DebugMarker.
//    %bar = void call @ext(%foo)
//       ; bar->DebugMarker = {
//       ;   StoredDbgRecords = {
//       ;     DbgVariableRecord(metadata i32 %foo, ...)
//       ;   }
//       ; }
//       ;; There is a debug-info record in front of the %bar instruction,
//       ;; thus it points at a DbgMarker object. That DbgMarker contains a
//       ;; DbgVariableRecord in its ilist, storing the equivalent information
//       ;; to the dbg.value above: the Value, DILocalVariable, etc.
//
// This structure separates the two concerns of the position of the debug-info
// in the function, and the Value that it refers to. It also creates a new
// "place" in-between the Value / Metadata hierachy where we can customise
// storage and allocation techniques to better suite debug-info workloads.
// NB: as of the initial prototype, none of that has actually been attempted
// yet.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_DEBUGPROGRAMINSTRUCTION_H
#define LLVM_IR_DEBUGPROGRAMINSTRUCTION_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/iterator.h"
#include "llvm/IR/DbgVariableFragmentInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/SymbolTableListTraits.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

class Instruction;
class BasicBlock;
class MDNode;
class Module;
class DbgVariableIntrinsic;
class DbgInfoIntrinsic;
class DbgLabelInst;
class DIAssignID;
class DbgMarker;
class DbgVariableRecord;
class raw_ostream;

/// A typed tracking MDNode reference that does not require a definition for its
/// parameter type. Necessary to avoid including DebugInfoMetadata.h, which has
/// a significant impact on compile times if included in this file.
template <typename T> class DbgRecordParamRef {
  TrackingMDNodeRef Ref;

public:
public:
  DbgRecordParamRef() = default;

  /// Construct from the templated type.
  DbgRecordParamRef(const T *Param);

  /// Construct from an \a MDNode.
  ///
  /// Note: if \c Param does not have the template type, a verifier check will
  /// fail, and accessors will crash.  However, construction from other nodes
  /// is supported in order to handle forward references when reading textual
  /// IR.
  explicit DbgRecordParamRef(const MDNode *Param);

  /// Get the underlying type.
  ///
  /// \pre !*this or \c isa<T>(getAsMDNode()).
  /// @{
  T *get() const;
  operator T *() const { return get(); }
  T *operator->() const { return get(); }
  T &operator*() const { return *get(); }
  /// @}

  /// Check for null.
  ///
  /// Check for null in a way that is safe with broken debug info.
  explicit operator bool() const { return Ref; }

  /// Return \c this as a \a MDNode.
  MDNode *getAsMDNode() const { return Ref; }

  bool operator==(const DbgRecordParamRef &Other) const {
    return Ref == Other.Ref;
  }
  bool operator!=(const DbgRecordParamRef &Other) const {
    return Ref != Other.Ref;
  }
};

extern template class LLVM_TEMPLATE_ABI DbgRecordParamRef<DIExpression>;
extern template class LLVM_TEMPLATE_ABI DbgRecordParamRef<DILabel>;
extern template class LLVM_TEMPLATE_ABI DbgRecordParamRef<DILocalVariable>;

/// Base class for non-instruction debug metadata records that have positions
/// within IR. Features various methods copied across from the Instruction
/// class to aid ease-of-use. DbgRecords should always be linked into a
/// DbgMarker's StoredDbgRecords list. The marker connects a DbgRecord back to
/// its position in the BasicBlock.
///
/// We need a discriminator for dyn/isa casts. In order to avoid paying for a
/// vtable for "virtual" functions too, subclasses must add a new discriminator
/// value (RecordKind) and cases to a few functions in the base class:
///   deleteRecord
///   clone
///   isIdenticalToWhenDefined
///   both print methods
///   createDebugIntrinsic
class DbgRecord : public ilist_node<DbgRecord> {
public:
  /// Marker that this DbgRecord is linked into.
  DbgMarker *Marker = nullptr;
  /// Subclass discriminator.
  enum Kind : uint8_t { ValueKind, LabelKind };

protected:
  DebugLoc DbgLoc;
  Kind RecordKind; ///< Subclass discriminator.

public:
  DbgRecord(Kind RecordKind, DebugLoc DL)
      : DbgLoc(DL), RecordKind(RecordKind) {}

  /// Methods that dispatch to subclass implementations. These need to be
  /// manually updated when a new subclass is added.
  ///@{
  LLVM_ABI void deleteRecord();
  LLVM_ABI DbgRecord *clone() const;
  LLVM_ABI void print(raw_ostream &O, bool IsForDebug = false) const;
  LLVM_ABI void print(raw_ostream &O, ModuleSlotTracker &MST,
                      bool IsForDebug) const;
  LLVM_ABI bool isIdenticalToWhenDefined(const DbgRecord &R) const;
  /// Convert this DbgRecord back into an appropriate llvm.dbg.* intrinsic.
  /// \p InsertBefore Optional position to insert this intrinsic.
  /// \returns A new llvm.dbg.* intrinsic representiung this DbgRecord.
  LLVM_ABI DbgInfoIntrinsic *
  createDebugIntrinsic(Module *M, Instruction *InsertBefore) const;
  ///@}

  /// Same as isIdenticalToWhenDefined but checks DebugLoc too.
  LLVM_ABI bool isEquivalentTo(const DbgRecord &R) const;

  Kind getRecordKind() const { return RecordKind; }

  void setMarker(DbgMarker *M) { Marker = M; }

  DbgMarker *getMarker() { return Marker; }
  const DbgMarker *getMarker() const { return Marker; }

  LLVM_ABI BasicBlock *getBlock();
  LLVM_ABI const BasicBlock *getBlock() const;

  LLVM_ABI Function *getFunction();
  LLVM_ABI const Function *getFunction() const;

  LLVM_ABI Module *getModule();
  LLVM_ABI const Module *getModule() const;

  LLVM_ABI LLVMContext &getContext();
  LLVM_ABI const LLVMContext &getContext() const;

  LLVM_ABI const Instruction *getInstruction() const;
  LLVM_ABI const BasicBlock *getParent() const;
  LLVM_ABI BasicBlock *getParent();

  LLVM_ABI void removeFromParent();
  LLVM_ABI void eraseFromParent();

  DbgRecord *getNextNode() { return &*std::next(getIterator()); }
  DbgRecord *getPrevNode() { return &*std::prev(getIterator()); }

  // Some generic lambdas supporting intrinsic-based debug-info mean we need
  // to support both iterator and instruction position based insertion.
  LLVM_ABI void insertBefore(DbgRecord *InsertBefore);
  LLVM_ABI void insertAfter(DbgRecord *InsertAfter);
  LLVM_ABI void moveBefore(DbgRecord *MoveBefore);
  LLVM_ABI void moveAfter(DbgRecord *MoveAfter);

  LLVM_ABI void insertBefore(self_iterator InsertBefore);
  LLVM_ABI void insertAfter(self_iterator InsertAfter);
  LLVM_ABI void moveBefore(self_iterator MoveBefore);
  LLVM_ABI void moveAfter(self_iterator MoveAfter);

  DebugLoc getDebugLoc() const { return DbgLoc; }
  void setDebugLoc(DebugLoc Loc) { DbgLoc = std::move(Loc); }

  LLVM_ABI void dump() const;

  using self_iterator = simple_ilist<DbgRecord>::iterator;
  using const_self_iterator = simple_ilist<DbgRecord>::const_iterator;

protected:
  /// Similarly to Value, we avoid paying the cost of a vtable
  /// by protecting the dtor and having deleteRecord dispatch
  /// cleanup.
  /// Use deleteRecord to delete a generic record.
  ~DbgRecord() = default;
};

inline raw_ostream &operator<<(raw_ostream &OS, const DbgRecord &R) {
  R.print(OS);
  return OS;
}

/// Records a position in IR for a source label (DILabel). Corresponds to the
/// llvm.dbg.label intrinsic.
class DbgLabelRecord : public DbgRecord {
  DbgRecordParamRef<DILabel> Label;

  /// This constructor intentionally left private, so that it is only called via
  /// "createUnresolvedDbgLabelRecord", which clearly expresses that it is for
  /// parsing only.
  DbgLabelRecord(MDNode *Label, MDNode *DL);

public:
  LLVM_ABI DbgLabelRecord(DILabel *Label, DebugLoc DL);

  /// For use during parsing; creates a DbgLabelRecord from as-of-yet unresolved
  /// MDNodes. Trying to access the resulting DbgLabelRecord's fields before
  /// they are resolved, or if they resolve to the wrong type, will result in a
  /// crash.
  LLVM_ABI static DbgLabelRecord *createUnresolvedDbgLabelRecord(MDNode *Label,
                                                                 MDNode *DL);

  LLVM_ABI DbgLabelRecord *clone() const;
  LLVM_ABI void print(raw_ostream &O, bool IsForDebug = false) const;
  LLVM_ABI void print(raw_ostream &ROS, ModuleSlotTracker &MST,
                      bool IsForDebug) const;
  LLVM_ABI DbgLabelInst *createDebugIntrinsic(Module *M,
                                              Instruction *InsertBefore) const;

  void setLabel(DILabel *NewLabel) { Label = NewLabel; }
  DILabel *getLabel() const { return Label.get(); }
  MDNode *getRawLabel() const { return Label.getAsMDNode(); };

  /// Support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DbgRecord *E) {
    return E->getRecordKind() == LabelKind;
  }
};

/// Record of a variable value-assignment, aka a non instruction representation
/// of the dbg.value intrinsic.
///
/// This class inherits from DebugValueUser to allow LLVM's metadata facilities
/// to update our references to metadata beneath our feet.
class DbgVariableRecord : public DbgRecord, protected DebugValueUser {
  friend class DebugValueUser;

public:
  enum class LocationType : uint8_t {
    Declare,
    Value,
    Assign,

    End, ///< Marks the end of the concrete types.
    Any, ///< To indicate all LocationTypes in searches.
  };
  /// Classification of the debug-info record that this DbgVariableRecord
  /// represents. Essentially, "does this correspond to a dbg.value,
  /// dbg.declare, or dbg.assign?".
  /// FIXME: We could use spare padding bits from DbgRecord for this.
  LocationType Type;

  // NB: there is no explicit "Value" field in this class, it's effectively the
  // DebugValueUser superclass instead. The referred to Value can either be a
  // ValueAsMetadata or a DIArgList.

  DbgRecordParamRef<DILocalVariable> Variable;
  DbgRecordParamRef<DIExpression> Expression;
  DbgRecordParamRef<DIExpression> AddressExpression;

public:
  /// Create a new DbgVariableRecord representing the intrinsic \p DVI, for
  /// example the assignment represented by a dbg.value.
  LLVM_ABI DbgVariableRecord(const DbgVariableIntrinsic *DVI);
  LLVM_ABI DbgVariableRecord(const DbgVariableRecord &DVR);
  /// Directly construct a new DbgVariableRecord representing a dbg.value
  /// intrinsic assigning \p Location to the DV / Expr / DI variable.
  LLVM_ABI DbgVariableRecord(Metadata *Location, DILocalVariable *DV,
                             DIExpression *Expr, const DILocation *DI,
                             LocationType Type = LocationType::Value);
  LLVM_ABI DbgVariableRecord(Metadata *Value, DILocalVariable *Variable,
                             DIExpression *Expression, DIAssignID *AssignID,
                             Metadata *Address, DIExpression *AddressExpression,
                             const DILocation *DI);

private:
  /// Private constructor for creating new instances during parsing only. Only
  /// called through `createUnresolvedDbgVariableRecord` below, which makes
  /// clear that this is used for parsing only, and will later return a subclass
  /// depending on which Type is passed.
  DbgVariableRecord(LocationType Type, Metadata *Val, MDNode *Variable,
                    MDNode *Expression, MDNode *AssignID, Metadata *Address,
                    MDNode *AddressExpression, MDNode *DI);

public:
  /// Used to create DbgVariableRecords during parsing, where some metadata
  /// references may still be unresolved. Although for some fields a generic
  /// `Metadata*` argument is accepted for forward type-references, the verifier
  /// and accessors will reject incorrect types later on. The function is used
  /// for all types of DbgVariableRecords for simplicity while parsing, but
  /// asserts if any necessary fields are empty or unused fields are not empty,
  /// i.e. if the #dbg_assign fields are used for a non-dbg-assign type.
  LLVM_ABI static DbgVariableRecord *
  createUnresolvedDbgVariableRecord(LocationType Type, Metadata *Val,
                                    MDNode *Variable, MDNode *Expression,
                                    MDNode *AssignID, Metadata *Address,
                                    MDNode *AddressExpression, MDNode *DI);

  LLVM_ABI static DbgVariableRecord *
  createDVRAssign(Value *Val, DILocalVariable *Variable,
                  DIExpression *Expression, DIAssignID *AssignID,
                  Value *Address, DIExpression *AddressExpression,
                  const DILocation *DI);
  LLVM_ABI static DbgVariableRecord *
  createLinkedDVRAssign(Instruction *LinkedInstr, Value *Val,
                        DILocalVariable *Variable, DIExpression *Expression,
                        Value *Address, DIExpression *AddressExpression,
                        const DILocation *DI);

  LLVM_ABI static DbgVariableRecord *
  createDbgVariableRecord(Value *Location, DILocalVariable *DV,
                          DIExpression *Expr, const DILocation *DI);
  LLVM_ABI static DbgVariableRecord *
  createDbgVariableRecord(Value *Location, DILocalVariable *DV,
                          DIExpression *Expr, const DILocation *DI,
                          DbgVariableRecord &InsertBefore);
  LLVM_ABI static DbgVariableRecord *createDVRDeclare(Value *Address,
                                                      DILocalVariable *DV,
                                                      DIExpression *Expr,
                                                      const DILocation *DI);
  LLVM_ABI static DbgVariableRecord *
  createDVRDeclare(Value *Address, DILocalVariable *DV, DIExpression *Expr,
                   const DILocation *DI, DbgVariableRecord &InsertBefore);

  /// Iterator for ValueAsMetadata that internally uses direct pointer iteration
  /// over either a ValueAsMetadata* or a ValueAsMetadata**, dereferencing to the
  /// ValueAsMetadata .
  class location_op_iterator
      : public iterator_facade_base<location_op_iterator,
                                    std::bidirectional_iterator_tag, Value *> {
    PointerUnion<ValueAsMetadata *, ValueAsMetadata **> I;

  public:
    location_op_iterator(ValueAsMetadata *SingleIter) : I(SingleIter) {}
    location_op_iterator(ValueAsMetadata **MultiIter) : I(MultiIter) {}

    location_op_iterator(const location_op_iterator &R) : I(R.I) {}
    location_op_iterator &operator=(const location_op_iterator &R) {
      I = R.I;
      return *this;
    }
    bool operator==(const location_op_iterator &RHS) const {
      return I == RHS.I;
    }
    const Value *operator*() const {
      ValueAsMetadata *VAM = isa<ValueAsMetadata *>(I)
                                 ? cast<ValueAsMetadata *>(I)
                                 : *cast<ValueAsMetadata **>(I);
      return VAM->getValue();
    };
    Value *operator*() {
      ValueAsMetadata *VAM = isa<ValueAsMetadata *>(I)
                                 ? cast<ValueAsMetadata *>(I)
                                 : *cast<ValueAsMetadata **>(I);
      return VAM->getValue();
    }
    location_op_iterator &operator++() {
      if (auto *VAM = dyn_cast<ValueAsMetadata *>(I))
        I = VAM + 1;
      else
        I = cast<ValueAsMetadata **>(I) + 1;
      return *this;
    }
    location_op_iterator &operator--() {
      if (auto *VAM = dyn_cast<ValueAsMetadata *>(I))
        I = VAM - 1;
      else
        I = cast<ValueAsMetadata **>(I) - 1;
      return *this;
    }
  };

  bool isDbgDeclare() const { return Type == LocationType::Declare; }
  bool isDbgValue() const { return Type == LocationType::Value; }

  /// Get the locations corresponding to the variable referenced by the debug
  /// info intrinsic.  Depending on the intrinsic, this could be the
  /// variable's value or its address.
  LLVM_ABI iterator_range<location_op_iterator> location_ops() const;

  LLVM_ABI Value *getVariableLocationOp(unsigned OpIdx) const;

  LLVM_ABI void replaceVariableLocationOp(Value *OldValue, Value *NewValue,
                                          bool AllowEmpty = false);
  LLVM_ABI void replaceVariableLocationOp(unsigned OpIdx, Value *NewValue);
  /// Adding a new location operand will always result in this intrinsic using
  /// an ArgList, and must always be accompanied by a new expression that uses
  /// the new operand.
  LLVM_ABI void addVariableLocationOps(ArrayRef<Value *> NewValues,
                                       DIExpression *NewExpr);

  LLVM_ABI unsigned getNumVariableLocationOps() const;

  bool hasArgList() const { return isa<DIArgList>(getRawLocation()); }
  /// Returns true if this DbgVariableRecord has no empty MDNodes in its
  /// location list.
  bool hasValidLocation() const { return getVariableLocationOp(0) != nullptr; }

  /// Does this describe the address of a local variable. True for dbg.addr
  /// and dbg.declare, but not dbg.value, which describes its value.
  bool isAddressOfVariable() const { return Type == LocationType::Declare; }

  /// Determine if this describes the value of a local variable. It is false for
  /// dbg.declare, but true for dbg.value, which describes its value.
  bool isValueOfVariable() const { return Type == LocationType::Value; }

  LocationType getType() const { return Type; }

  LLVM_ABI void setKillLocation();
  LLVM_ABI bool isKillLocation() const;

  void setVariable(DILocalVariable *NewVar) { Variable = NewVar; }
  DILocalVariable *getVariable() const { return Variable.get(); };
  MDNode *getRawVariable() const { return Variable.getAsMDNode(); }

  void setExpression(DIExpression *NewExpr) { Expression = NewExpr; }
  DIExpression *getExpression() const { return Expression.get(); }
  MDNode *getRawExpression() const { return Expression.getAsMDNode(); }

  /// Returns the metadata operand for the first location description. i.e.,
  /// dbg intrinsic dbg.value,declare operand and dbg.assign 1st location
  /// operand (the "value componenet"). Note the operand (singular) may be
  /// a DIArgList which is a list of values.
  Metadata *getRawLocation() const { return DebugValues[0]; }

  Value *getValue(unsigned OpIdx = 0) const {
    return getVariableLocationOp(OpIdx);
  }

  /// Use of this should generally be avoided; instead,
  /// replaceVariableLocationOp and addVariableLocationOps should be used where
  /// possible to avoid creating invalid state.
  void setRawLocation(Metadata *NewLocation) {
    assert((isa<ValueAsMetadata>(NewLocation) || isa<DIArgList>(NewLocation) ||
            isa<MDNode>(NewLocation)) &&
           "Location for a DbgVariableRecord must be either ValueAsMetadata or "
           "DIArgList");
    resetDebugValue(0, NewLocation);
  }

  LLVM_ABI std::optional<DbgVariableFragmentInfo> getFragment() const;
  /// Get the FragmentInfo for the variable if it exists, otherwise return a
  /// FragmentInfo that covers the entire variable if the variable size is
  /// known, otherwise return a zero-sized fragment.
  DbgVariableFragmentInfo getFragmentOrEntireVariable() const {
    if (auto Frag = getFragment())
      return *Frag;
    if (auto Sz = getFragmentSizeInBits())
      return {*Sz, 0};
    return {0, 0};
  }
  /// Get the size (in bits) of the variable, or fragment of the variable that
  /// is described.
  LLVM_ABI std::optional<uint64_t> getFragmentSizeInBits() const;

  bool isEquivalentTo(const DbgVariableRecord &Other) const {
    return DbgLoc == Other.DbgLoc && isIdenticalToWhenDefined(Other);
  }
  // Matches the definition of the Instruction version, equivalent to above but
  // without checking DbgLoc.
  bool isIdenticalToWhenDefined(const DbgVariableRecord &Other) const {
    return std::tie(Type, DebugValues, Variable, Expression,
                    AddressExpression) ==
           std::tie(Other.Type, Other.DebugValues, Other.Variable,
                    Other.Expression, Other.AddressExpression);
  }

  /// @name DbgAssign Methods
  /// @{
  bool isDbgAssign() const { return getType() == LocationType::Assign; }

  LLVM_ABI Value *getAddress() const;
  Metadata *getRawAddress() const {
    return isDbgAssign() ? DebugValues[1] : DebugValues[0];
  }
  Metadata *getRawAssignID() const { return DebugValues[2]; }
  LLVM_ABI DIAssignID *getAssignID() const;
  DIExpression *getAddressExpression() const { return AddressExpression.get(); }
  MDNode *getRawAddressExpression() const {
    return AddressExpression.getAsMDNode();
  }
  void setAddressExpression(DIExpression *NewExpr) {
    AddressExpression = NewExpr;
  }
  LLVM_ABI void setAssignId(DIAssignID *New);
  void setAddress(Value *V) { resetDebugValue(1, ValueAsMetadata::get(V)); }
  /// Kill the address component.
  LLVM_ABI void setKillAddress();
  /// Check whether this kills the address component. This doesn't take into
  /// account the position of the intrinsic, therefore a returned value of false
  /// does not guarentee the address is a valid location for the variable at the
  /// intrinsic's position in IR.
  LLVM_ABI bool isKillAddress() const;

  /// @}

  LLVM_ABI DbgVariableRecord *clone() const;
  /// Convert this DbgVariableRecord back into a dbg.value intrinsic.
  /// \p InsertBefore Optional position to insert this intrinsic.
  /// \returns A new dbg.value intrinsic representiung this DbgVariableRecord.
  LLVM_ABI DbgVariableIntrinsic *
  createDebugIntrinsic(Module *M, Instruction *InsertBefore) const;

  /// Handle changes to the location of the Value(s) that we refer to happening
  /// "under our feet".
  LLVM_ABI void handleChangedLocation(Metadata *NewLocation);

  LLVM_ABI void print(raw_ostream &O, bool IsForDebug = false) const;
  LLVM_ABI void print(raw_ostream &ROS, ModuleSlotTracker &MST,
                      bool IsForDebug) const;

  /// Support type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DbgRecord *E) {
    return E->getRecordKind() == ValueKind;
  }
};

/// Filter the DbgRecord range to DbgVariableRecord types only and downcast.
static inline auto
filterDbgVars(iterator_range<simple_ilist<DbgRecord>::iterator> R) {
  return map_range(
      make_filter_range(R,
                        [](DbgRecord &E) { return isa<DbgVariableRecord>(E); }),
      [](DbgRecord &E) { return std::ref(cast<DbgVariableRecord>(E)); });
}

/// Per-instruction record of debug-info. If an Instruction is the position of
/// some debugging information, it points at a DbgMarker storing that info. Each
/// marker points back at the instruction that owns it. Various utilities are
/// provided for manipulating the DbgRecords contained within this marker.
///
/// This class has a rough surface area, because it's needed to preserve the
/// one arefact that we can't yet eliminate from the intrinsic / dbg.value
/// debug-info design: the order of records is significant, and duplicates can
/// exist. Thus, if one has a run of debug-info records such as:
///    dbg.value(...
///    %foo = barinst
///    dbg.value(...
/// and remove barinst, then the dbg.values must be preserved in the correct
/// order. Hence, the use of iterators to select positions to insert things
/// into, or the occasional InsertAtHead parameter indicating that new records
/// should go at the start of the list.
///
/// There are only five or six places in LLVM that truly rely on this ordering,
/// which we can improve in the future. Additionally, many improvements in the
/// way that debug-info is stored can be achieved in this class, at a future
/// date.
class DbgMarker {
public:
  DbgMarker() {}
  /// Link back to the Instruction that owns this marker. Can be null during
  /// operations that move a marker from one instruction to another.
  Instruction *MarkedInstr = nullptr;

  /// List of DbgRecords, the non-instruction equivalent of llvm.dbg.*
  /// intrinsics. There is a one-to-one relationship between each debug
  /// intrinsic in a block and each DbgRecord once the representation has been
  /// converted, and the ordering is meaningful in the same way.
  simple_ilist<DbgRecord> StoredDbgRecords;
  bool empty() const { return StoredDbgRecords.empty(); }

  LLVM_ABI const BasicBlock *getParent() const;
  LLVM_ABI BasicBlock *getParent();

  /// Handle the removal of a marker: the position of debug-info has gone away,
  /// but the stored debug records should not. Drop them onto the next
  /// instruction, or otherwise work out what to do with them.
  LLVM_ABI void removeMarker();
  LLVM_ABI void dump() const;

  LLVM_ABI void removeFromParent();
  LLVM_ABI void eraseFromParent();

  /// Implement operator<< on DbgMarker.
  LLVM_ABI void print(raw_ostream &O, bool IsForDebug = false) const;
  LLVM_ABI void print(raw_ostream &ROS, ModuleSlotTracker &MST,
                      bool IsForDebug) const;

  /// Produce a range over all the DbgRecords in this Marker.
  LLVM_ABI iterator_range<simple_ilist<DbgRecord>::iterator>
  getDbgRecordRange();
  LLVM_ABI iterator_range<simple_ilist<DbgRecord>::const_iterator>
  getDbgRecordRange() const;
  /// Transfer any DbgRecords from \p Src into this DbgMarker. If \p
  /// InsertAtHead is true, place them before existing DbgRecords, otherwise
  /// afterwards.
  LLVM_ABI void absorbDebugValues(DbgMarker &Src, bool InsertAtHead);
  /// Transfer the DbgRecords in \p Range from \p Src into this DbgMarker. If
  /// \p InsertAtHead is true, place them before existing DbgRecords, otherwise
  // afterwards.
  LLVM_ABI void
  absorbDebugValues(iterator_range<DbgRecord::self_iterator> Range,
                    DbgMarker &Src, bool InsertAtHead);
  /// Insert a DbgRecord into this DbgMarker, at the end of the list. If
  /// \p InsertAtHead is true, at the start.
  LLVM_ABI void insertDbgRecord(DbgRecord *New, bool InsertAtHead);
  /// Insert a DbgRecord prior to a DbgRecord contained within this marker.
  LLVM_ABI void insertDbgRecord(DbgRecord *New, DbgRecord *InsertBefore);
  /// Insert a DbgRecord after a DbgRecord contained within this marker.
  LLVM_ABI void insertDbgRecordAfter(DbgRecord *New, DbgRecord *InsertAfter);
  /// Clone all DbgMarkers from \p From into this marker. There are numerous
  /// options to customise the source/destination, due to gnarliness, see class
  /// comment.
  /// \p FromHere If non-null, copy from FromHere to the end of From's
  /// DbgRecords
  /// \p InsertAtHead Place the cloned DbgRecords at the start of
  /// StoredDbgRecords
  /// \returns Range over all the newly cloned DbgRecords
  LLVM_ABI iterator_range<simple_ilist<DbgRecord>::iterator>
  cloneDebugInfoFrom(DbgMarker *From,
                     std::optional<simple_ilist<DbgRecord>::iterator> FromHere,
                     bool InsertAtHead = false);
  /// Erase all DbgRecords in this DbgMarker.
  LLVM_ABI void dropDbgRecords();
  /// Erase a single DbgRecord from this marker. In an ideal future, we would
  /// never erase an assignment in this way, but it's the equivalent to
  /// erasing a debug intrinsic from a block.
  LLVM_ABI void dropOneDbgRecord(DbgRecord *DR);

  /// We generally act like all llvm Instructions have a range of DbgRecords
  /// attached to them, but in reality sometimes we don't allocate the DbgMarker
  /// to save time and memory, but still have to return ranges of DbgRecords.
  /// When we need to describe such an unallocated DbgRecord range, use this
  /// static markers range instead. This will bite us if someone tries to insert
  /// a DbgRecord in that range, but they should be using the Official (TM) API
  /// for that.
  LLVM_ABI static DbgMarker EmptyDbgMarker;
  static iterator_range<simple_ilist<DbgRecord>::iterator>
  getEmptyDbgRecordRange() {
    return make_range(EmptyDbgMarker.StoredDbgRecords.end(),
                      EmptyDbgMarker.StoredDbgRecords.end());
  }
};

inline raw_ostream &operator<<(raw_ostream &OS, const DbgMarker &Marker) {
  Marker.print(OS);
  return OS;
}

/// Inline helper to return a range of DbgRecords attached to a marker. It needs
/// to be inlined as it's frequently called, but also come after the declaration
/// of DbgMarker. Thus: it's pre-declared by users like Instruction, then an
/// inlineable body defined here.
inline iterator_range<simple_ilist<DbgRecord>::iterator>
getDbgRecordRange(DbgMarker *DebugMarker) {
  if (!DebugMarker)
    return DbgMarker::getEmptyDbgRecordRange();
  return DebugMarker->getDbgRecordRange();
}

DEFINE_ISA_CONVERSION_FUNCTIONS(DbgRecord, LLVMDbgRecordRef)

} // namespace llvm

#endif // LLVM_IR_DEBUGPROGRAMINSTRUCTION_H
