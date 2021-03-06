// Copyright 2006-2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <stdlib.h>

#include "v8.h"

#include "scopeinfo.h"
#include "scopes.h"

namespace v8 {
namespace internal {


static int CompareLocal(Variable* const* v, Variable* const* w) {
  Slot* s = (*v)->AsSlot();
  Slot* t = (*w)->AsSlot();
  // We may have rewritten parameters (that are in the arguments object)
  // and which may have a NULL slot... - find a better solution...
  int x = (s != NULL ? s->index() : 0);
  int y = (t != NULL ? t->index() : 0);
  // Consider sorting them according to type as well?
  return x - y;
}


template<class Allocator>
ScopeInfo<Allocator>::ScopeInfo(Scope* scope)
    : function_name_(Factory::empty_symbol()),
      calls_eval_(scope->calls_eval()),
      parameters_(scope->num_parameters()),
      stack_slots_(scope->num_stack_slots()),
      context_slots_(scope->num_heap_slots()),
      context_modes_(scope->num_heap_slots()) {
  // Add parameters.
  for (int i = 0; i < scope->num_parameters(); i++) {
    ASSERT(parameters_.length() == i);
    parameters_.Add(scope->parameter(i)->name());
  }

  // Add stack locals and collect heap locals.
  // We are assuming that the locals' slots are allocated in
  // increasing order, so we can simply add them to the
  // ScopeInfo lists. However, due to usage analysis, this is
  // not true for context-allocated locals: Some of them
  // may be parameters which are allocated before the
  // non-parameter locals. When the non-parameter locals are
  // sorted according to usage, the allocated slot indices may
  // not be in increasing order with the variable list anymore.
  // Thus, we first collect the context-allocated locals, and then
  // sort them by context slot index before adding them to the
  // ScopeInfo list.
  List<Variable*, Allocator> locals(32);  // 32 is a wild guess
  ASSERT(locals.is_empty());
  scope->CollectUsedVariables(&locals);
  locals.Sort(&CompareLocal);

  List<Variable*, Allocator> heap_locals(locals.length());
  for (int i = 0; i < locals.length(); i++) {
    Variable* var = locals[i];
    if (var->is_used()) {
      Slot* slot = var->AsSlot();
      if (slot != NULL) {
        switch (slot->type()) {
          case Slot::PARAMETER:
            // explicitly added to parameters_ above - ignore
            break;

          case Slot::LOCAL:
            ASSERT(stack_slots_.length() == slot->index());
            stack_slots_.Add(var->name());
            break;

          case Slot::CONTEXT:
            heap_locals.Add(var);
            break;

          case Slot::LOOKUP:
            // This is currently not used.
            UNREACHABLE();
            break;
        }
      }
    }
  }

  // Add heap locals.
  if (scope->num_heap_slots() > 0) {
    // Add user-defined slots.
    for (int i = 0; i < heap_locals.length(); i++) {
      ASSERT(heap_locals[i]->AsSlot()->index() - Context::MIN_CONTEXT_SLOTS ==
             context_slots_.length());
      ASSERT(heap_locals[i]->AsSlot()->index() - Context::MIN_CONTEXT_SLOTS ==
             context_modes_.length());
      context_slots_.Add(heap_locals[i]->name());
      context_modes_.Add(heap_locals[i]->mode());
    }

  } else {
    ASSERT(heap_locals.length() == 0);
  }

  // Add the function context slot, if present.
  // For now, this must happen at the very end because of the
  // ordering of the scope info slots and the respective slot indices.
  if (scope->is_function_scope()) {
    Variable* var = scope->function();
    if (var != NULL &&
        var->is_used() &&
        var->AsSlot()->type() == Slot::CONTEXT) {
      function_name_ = var->name();
      // Note that we must not find the function name in the context slot
      // list - instead it must be handled separately in the
      // Contexts::Lookup() function. Thus record an empty symbol here so we
      // get the correct number of context slots.
      ASSERT(var->AsSlot()->index() - Context::MIN_CONTEXT_SLOTS ==
             context_slots_.length());
      ASSERT(var->AsSlot()->index() - Context::MIN_CONTEXT_SLOTS ==
             context_modes_.length());
      context_slots_.Add(Factory::empty_symbol());
      context_modes_.Add(Variable::INTERNAL);
    }
  }
}


// Encoding format in a FixedArray object:
//
// - function name
//
// - calls eval boolean flag
//
// - number of variables in the context object (smi) (= function context
//   slot index + 1)
// - list of pairs (name, Var mode) of context-allocated variables (starting
//   with context slot 0)
//
// - number of parameters (smi)
// - list of parameter names (starting with parameter 0 first)
//
// - number of variables on the stack (smi)
// - list of names of stack-allocated variables (starting with stack slot 0)

// The ScopeInfo representation could be simplified and the ScopeInfo
// re-implemented (with almost the same interface). Here is a
// suggestion for the new format:
//
// - have a single list with all variable names (parameters, stack locals,
//   context locals), followed by a list of non-Object* values containing
//   the variables information (what kind, index, attributes)
// - searching the linear list of names is fast and yields an index into the
//   list if the variable name is found
// - that list index is then used to find the variable information in the
//   subsequent list
// - the list entries don't have to be in any particular order, so all the
//   current sorting business can go away
// - the ScopeInfo lookup routines can be reduced to perhaps a single lookup
//   which returns all information at once
// - when gathering the information from a Scope, we only need to iterate
//   through the local variables (parameters and context info is already
//   present)


static inline Object** ReadInt(Object** p, int* x) {
  *x = (reinterpret_cast<Smi*>(*p++))->value();
  return p;
}


static inline Object** ReadBool(Object** p, bool* x) {
  *x = (reinterpret_cast<Smi*>(*p++))->value() != 0;
  return p;
}


static inline Object** ReadSymbol(Object** p, Handle<String>* s) {
  *s = Handle<String>(reinterpret_cast<String*>(*p++));
  return p;
}


template <class Allocator>
static Object** ReadList(Object** p, List<Handle<String>, Allocator >* list) {
  ASSERT(list->is_empty());
  int n;
  p = ReadInt(p, &n);
  while (n-- > 0) {
    Handle<String> s;
    p = ReadSymbol(p, &s);
    list->Add(s);
  }
  return p;
}


template <class Allocator>
static Object** ReadList(Object** p,
                         List<Handle<String>, Allocator>* list,
                         List<Variable::Mode, Allocator>* modes) {
  ASSERT(list->is_empty());
  int n;
  p = ReadInt(p, &n);
  while (n-- > 0) {
    Handle<String> s;
    int m;
    p = ReadSymbol(p, &s);
    p = ReadInt(p, &m);
    list->Add(s);
    modes->Add(static_cast<Variable::Mode>(m));
  }
  return p;
}


template<class Allocator>
ScopeInfo<Allocator>::ScopeInfo(SerializedScopeInfo* data)
  : function_name_(Factory::empty_symbol()),
    parameters_(4),
    stack_slots_(8),
    context_slots_(8),
    context_modes_(8) {
  if (data->length() > 0) {
    Object** p0 = data->data_start();
    Object** p = p0;
    p = ReadSymbol(p, &function_name_);
    p = ReadBool(p, &calls_eval_);
    p = ReadList<Allocator>(p, &context_slots_, &context_modes_);
    p = ReadList<Allocator>(p, &parameters_);
    p = ReadList<Allocator>(p, &stack_slots_);
    ASSERT((p - p0) == FixedArray::cast(data)->length());
  }
}


static inline Object** WriteInt(Object** p, int x) {
  *p++ = Smi::FromInt(x);
  return p;
}


static inline Object** WriteBool(Object** p, bool b) {
  *p++ = Smi::FromInt(b ? 1 : 0);
  return p;
}


static inline Object** WriteSymbol(Object** p, Handle<String> s) {
  *p++ = *s;
  return p;
}


template <class Allocator>
static Object** WriteList(Object** p, List<Handle<String>, Allocator >* list) {
  const int n = list->length();
  p = WriteInt(p, n);
  for (int i = 0; i < n; i++) {
    p = WriteSymbol(p, list->at(i));
  }
  return p;
}


template <class Allocator>
static Object** WriteList(Object** p,
                          List<Handle<String>, Allocator>* list,
                          List<Variable::Mode, Allocator>* modes) {
  const int n = list->length();
  p = WriteInt(p, n);
  for (int i = 0; i < n; i++) {
    p = WriteSymbol(p, list->at(i));
    p = WriteInt(p, modes->at(i));
  }
  return p;
}


template<class Allocator>
Handle<SerializedScopeInfo> ScopeInfo<Allocator>::Serialize() {
  // function name, calls eval, length for 3 tables:
  const int extra_slots = 1 + 1 + 3;
  int length = extra_slots +
               context_slots_.length() * 2 +
               parameters_.length() +
               stack_slots_.length();

  Handle<SerializedScopeInfo> data(
      SerializedScopeInfo::cast(*Factory::NewFixedArray(length, TENURED)));
  AssertNoAllocation nogc;

  Object** p0 = data->data_start();
  Object** p = p0;
  p = WriteSymbol(p, function_name_);
  p = WriteBool(p, calls_eval_);
  p = WriteList(p, &context_slots_, &context_modes_);
  p = WriteList(p, &parameters_);
  p = WriteList(p, &stack_slots_);
  ASSERT((p - p0) == length);

  return data;
}


template<class Allocator>
Handle<String> ScopeInfo<Allocator>::LocalName(int i) const {
  // A local variable can be allocated either on the stack or in the context.
  // For variables allocated in the context they are always preceded by
  // Context::MIN_CONTEXT_SLOTS of fixed allocated slots in the context.
  if (i < number_of_stack_slots()) {
    return stack_slot_name(i);
  } else {
    return context_slot_name(i - number_of_stack_slots() +
                             Context::MIN_CONTEXT_SLOTS);
  }
}


template<class Allocator>
int ScopeInfo<Allocator>::NumberOfLocals() const {
  int number_of_locals = number_of_stack_slots();
  if (number_of_context_slots() > 0) {
    ASSERT(number_of_context_slots() >= Context::MIN_CONTEXT_SLOTS);
    number_of_locals += number_of_context_slots() - Context::MIN_CONTEXT_SLOTS;
  }
  return number_of_locals;
}


Handle<SerializedScopeInfo> SerializedScopeInfo::Create(Scope* scope) {
  ScopeInfo<ZoneListAllocationPolicy> sinfo(scope);
  return sinfo.Serialize();
}


SerializedScopeInfo* SerializedScopeInfo::Empty() {
  return reinterpret_cast<SerializedScopeInfo*>(Heap::empty_fixed_array());
}


Object** SerializedScopeInfo::ContextEntriesAddr() {
  ASSERT(length() > 0);
  return data_start() + 2;  // +2 for function name and calls eval.
}


Object** SerializedScopeInfo::ParameterEntriesAddr() {
  ASSERT(length() > 0);
  Object** p = ContextEntriesAddr();
  int number_of_context_slots;
  p = ReadInt(p, &number_of_context_slots);
  return p + number_of_context_slots*2;  // *2 for pairs
}


Object** SerializedScopeInfo::StackSlotEntriesAddr() {
  ASSERT(length() > 0);
  Object** p = ParameterEntriesAddr();
  int number_of_parameter_slots;
  p = ReadInt(p, &number_of_parameter_slots);
  return p + number_of_parameter_slots;
}


bool SerializedScopeInfo::CallsEval() {
  if (length() > 0) {
    Object** p = data_start() + 1;  // +1 for function name.
    bool calls_eval;
    p = ReadBool(p, &calls_eval);
    return calls_eval;
  }
  return true;
}


int SerializedScopeInfo::NumberOfStackSlots() {
  if (length() > 0) {
    Object** p = StackSlotEntriesAddr();
    int number_of_stack_slots;
    ReadInt(p, &number_of_stack_slots);
    return number_of_stack_slots;
  }
  return 0;
}


int SerializedScopeInfo::NumberOfContextSlots() {
  if (length() > 0) {
    Object** p = ContextEntriesAddr();
    int number_of_context_slots;
    ReadInt(p, &number_of_context_slots);
    return number_of_context_slots + Context::MIN_CONTEXT_SLOTS;
  }
  return 0;
}


bool SerializedScopeInfo::HasHeapAllocatedLocals() {
  if (length() > 0) {
    Object** p = ContextEntriesAddr();
    int number_of_context_slots;
    ReadInt(p, &number_of_context_slots);
    return number_of_context_slots > 0;
  }
  return false;
}


int SerializedScopeInfo::StackSlotIndex(String* name) {
  ASSERT(name->IsSymbol());
  if (length() > 0) {
    // Slots start after length entry.
    Object** p0 = StackSlotEntriesAddr();
    int number_of_stack_slots;
    p0 = ReadInt(p0, &number_of_stack_slots);
    Object** p = p0;
    Object** end = p0 + number_of_stack_slots;
    while (p != end) {
      if (*p == name) return static_cast<int>(p - p0);
      p++;
    }
  }
  return -1;
}

int SerializedScopeInfo::ContextSlotIndex(String* name, Variable::Mode* mode) {
  ASSERT(name->IsSymbol());
  int result = ContextSlotCache::Lookup(this, name, mode);
  if (result != ContextSlotCache::kNotFound) return result;
  if (length() > 0) {
    // Slots start after length entry.
    Object** p0 = ContextEntriesAddr();
    int number_of_context_slots;
    p0 = ReadInt(p0, &number_of_context_slots);
    Object** p = p0;
    Object** end = p0 + number_of_context_slots * 2;
    while (p != end) {
      if (*p == name) {
        ASSERT(((p - p0) & 1) == 0);
        int v;
        ReadInt(p + 1, &v);
        Variable::Mode mode_value = static_cast<Variable::Mode>(v);
        if (mode != NULL) *mode = mode_value;
        result = static_cast<int>((p - p0) >> 1) + Context::MIN_CONTEXT_SLOTS;
        ContextSlotCache::Update(this, name, mode_value, result);
        return result;
      }
      p += 2;
    }
  }
  ContextSlotCache::Update(this, name, Variable::INTERNAL, -1);
  return -1;
}


int SerializedScopeInfo::ParameterIndex(String* name) {
  ASSERT(name->IsSymbol());
  if (length() > 0) {
    // We must read parameters from the end since for
    // multiply declared parameters the value of the
    // last declaration of that parameter is used
    // inside a function (and thus we need to look
    // at the last index). Was bug# 1110337.
    //
    // Eventually, we should only register such parameters
    // once, with corresponding index. This requires a new
    // implementation of the ScopeInfo code. See also other
    // comments in this file regarding this.
    Object** p = ParameterEntriesAddr();
    int number_of_parameter_slots;
    Object** p0 = ReadInt(p, &number_of_parameter_slots);
    p = p0 + number_of_parameter_slots;
    while (p > p0) {
      p--;
      if (*p == name) return static_cast<int>(p - p0);
    }
  }
  return -1;
}


int SerializedScopeInfo::FunctionContextSlotIndex(String* name) {
  ASSERT(name->IsSymbol());
  if (length() > 0) {
    Object** p = data_start();
    if (*p == name) {
      p = ContextEntriesAddr();
      int number_of_context_slots;
      ReadInt(p, &number_of_context_slots);
      ASSERT(number_of_context_slots != 0);
      // The function context slot is the last entry.
      return number_of_context_slots + Context::MIN_CONTEXT_SLOTS - 1;
    }
  }
  return -1;
}


int ContextSlotCache::Hash(Object* data, String* name) {
  // Uses only lower 32 bits if pointers are larger.
  uintptr_t addr_hash =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data)) >> 2;
  return static_cast<int>((addr_hash ^ name->Hash()) % kLength);
}


int ContextSlotCache::Lookup(Object* data,
                             String* name,
                             Variable::Mode* mode) {
  int index = Hash(data, name);
  Key& key = keys_[index];
  if ((key.data == data) && key.name->Equals(name)) {
    Value result(values_[index]);
    if (mode != NULL) *mode = result.mode();
    return result.index() + kNotFound;
  }
  return kNotFound;
}


void ContextSlotCache::Update(Object* data,
                              String* name,
                              Variable::Mode mode,
                              int slot_index) {
  String* symbol;
  ASSERT(slot_index > kNotFound);
  if (Heap::LookupSymbolIfExists(name, &symbol)) {
    int index = Hash(data, symbol);
    Key& key = keys_[index];
    key.data = data;
    key.name = symbol;
    // Please note value only takes a uint as index.
    values_[index] = Value(mode, slot_index - kNotFound).raw();
#ifdef DEBUG
    ValidateEntry(data, name, mode, slot_index);
#endif
  }
}


void ContextSlotCache::Clear() {
  for (int index = 0; index < kLength; index++) keys_[index].data = NULL;
}


ContextSlotCache::Key ContextSlotCache::keys_[ContextSlotCache::kLength];


uint32_t ContextSlotCache::values_[ContextSlotCache::kLength];


#ifdef DEBUG

void ContextSlotCache::ValidateEntry(Object* data,
                                     String* name,
                                     Variable::Mode mode,
                                     int slot_index) {
  String* symbol;
  if (Heap::LookupSymbolIfExists(name, &symbol)) {
    int index = Hash(data, name);
    Key& key = keys_[index];
    ASSERT(key.data == data);
    ASSERT(key.name->Equals(name));
    Value result(values_[index]);
    ASSERT(result.mode() == mode);
    ASSERT(result.index() + kNotFound == slot_index);
  }
}


template <class Allocator>
static void PrintList(const char* list_name,
                      int nof_internal_slots,
                      List<Handle<String>, Allocator>& list) {
  if (list.length() > 0) {
    PrintF("\n  // %s\n", list_name);
    if (nof_internal_slots > 0) {
      PrintF("  %2d - %2d [internal slots]\n", 0 , nof_internal_slots - 1);
    }
    for (int i = 0; i < list.length(); i++) {
      PrintF("  %2d ", i + nof_internal_slots);
      list[i]->ShortPrint();
      PrintF("\n");
    }
  }
}


template<class Allocator>
void ScopeInfo<Allocator>::Print() {
  PrintF("ScopeInfo ");
  if (function_name_->length() > 0)
    function_name_->ShortPrint();
  else
    PrintF("/* no function name */");
  PrintF("{");

  PrintList<Allocator>("parameters", 0, parameters_);
  PrintList<Allocator>("stack slots", 0, stack_slots_);
  PrintList<Allocator>("context slots", Context::MIN_CONTEXT_SLOTS,
                       context_slots_);

  PrintF("}\n");
}
#endif  // DEBUG


// Make sure the classes get instantiated by the template system.
template class ScopeInfo<FreeStoreAllocationPolicy>;
template class ScopeInfo<PreallocatedStorage>;
template class ScopeInfo<ZoneListAllocationPolicy>;

} }  // namespace v8::internal
