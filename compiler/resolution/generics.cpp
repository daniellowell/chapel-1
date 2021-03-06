/*
 * Copyright 2004-2017 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "resolution.h"

#include "astutil.h"
#include "caches.h"
#include "chpl.h"
#include "expr.h"
#include "stmt.h"
#include "stringutil.h"
#include "symbol.h"

#include "resolveIntents.h"

#include <cstdlib>
#include <inttypes.h>

int                    explainInstantiationLine   = -2;
ModuleSymbol*          explainInstantiationModule = NULL;
static Vec<FnSymbol*>  whereStack;

void
explainInstantiation(FnSymbol* fn) {
  if (strcmp(fn->name, fExplainInstantiation) &&
      (strncmp(fn->name, "_construct_", 11) ||
       strcmp(fn->name+11, fExplainInstantiation)))
    return;
  if (explainInstantiationModule && explainInstantiationModule != fn->defPoint->getModule())
    return;
  if (explainInstantiationLine != -1 && explainInstantiationLine != fn->defPoint->linenum())
    return;

  char msg[1024] = "";
  int len;
  if (fn->hasFlag(FLAG_CONSTRUCTOR))
    len = sprintf(msg, "instantiated %s(", fn->_this->type->symbol->name);
  else
    len = sprintf(msg, "instantiated %s(", fn->name);
  bool first = true;
  for_formals(formal, fn) {
    form_Map(SymbolMapElem, e, fn->substitutions) {
      ArgSymbol* arg = toArgSymbol(e->key);
      if (!strcmp(formal->name, arg->name)) {
        if (arg->hasFlag(FLAG_IS_MEME)) // do not show meme argument
          continue;
        if (first)
          first = false;
        else
          len += sprintf(msg+len, ", ");
        INT_ASSERT(arg);
        if (strcmp(fn->name, "_construct__tuple"))
          len += sprintf(msg+len, "%s = ", arg->name);
        if (VarSymbol* vs = toVarSymbol(e->value)) {
          if (vs->immediate && vs->immediate->const_kind == NUM_KIND_INT)
            len += sprintf(msg+len, "%" PRId64, vs->immediate->int_value());
          else if (vs->immediate && vs->immediate->const_kind == CONST_KIND_STRING)
            len += sprintf(msg+len, "\"%s\"", vs->immediate->v_string);
          else
            len += sprintf(msg+len, "%s", vs->name);
        }
        else if (Symbol* s = toSymbol(e->value))
      // For a generic symbol, just print the name.
      // Additional clauses for specific symbol types should precede this one.
          len += sprintf(msg+len, "%s", s->name);
        else
          INT_FATAL("unexpected case using --explain-instantiation");
      }
    }
  }
  sprintf(msg+len, ")");
  if (callStack.n) {
    USR_PRINT(callStack.v[callStack.n-1], msg);
  } else {
    USR_PRINT(fn, msg);
  }
}


void
copyGenericSub(SymbolMap& subs, FnSymbol* root, FnSymbol* fn, Symbol* key, Symbol* value) {
  if (!strcmp("_type_construct__tuple", root->name) && key->name[0] == 'x') {
    subs.put(new_IntSymbol(atoi(key->name+1)), value);
  } else if (root != fn) {
    int i = 1;
    for_formals(formal, fn) {
      if (formal == key) {
        subs.put(root->getFormal(i), value);
      }
      i++;
    }
  } else {
    subs.put(key, value);
  }
}

TypeSymbol*
getNewSubType(FnSymbol* fn, Symbol* key, TypeSymbol* actualTS) {
  if (fn->hasEitherFlag(FLAG_TUPLE,FLAG_PARTIAL_TUPLE)) {
    return actualTS;
  } else if (fn->hasFlag(FLAG_ALLOW_REF)) {
    // With FLAG_ALLOW_REF, always use actualTS type, even if it's a ref type
    return actualTS;
  } else if (fn->hasFlag(FLAG_REF)) {
    // With FLAG_REF on the function, that means it's a constructor
    // for the ref type, so re-instantiate it with whatever actualTS is.
    return actualTS;
  } else {
    bool actualRef = actualTS->hasFlag(FLAG_REF);

    if(actualRef)
      // the value is a ref and
      // instantiation of a formal of ref type loses ref
      return getNewSubType(fn, key, actualTS->getValType()->symbol);
    else
      return actualTS;
  }
}


void
checkInfiniteWhereInstantiation(FnSymbol* fn) {
  if (fn->where) {
    forv_Vec(FnSymbol, where, whereStack) {
      if (where == fn) {
        USR_FATAL_CONT(fn->where, "illegal where clause due"
                       " to infinite instantiation");
        FnSymbol* printOn = NULL;
        forv_Vec(FnSymbol, tmp, whereStack) {
          if (printOn)
            USR_PRINT(printOn->where, "evaluation of '%s' where clause results"
                      " in instantiation of '%s'", printOn->name, tmp->name);
          if (printOn || tmp == where)
            printOn = tmp;
        }
        USR_PRINT(fn->where, "evaluation of '%s' where clause results"
                  " in instantiation of '%s'", printOn->name, fn->name);
        USR_STOP();
      }
    }
  }
}


//
// check for infinite instantiation by limiting the number of
// instantiations of a particular type or function; this is important
// so as to contain cases like def foo(param p: int) return foo(p+1);
//
// note that this check is disabled for functions in the base module
// because folding is done via instantiation; therefore, be careful
// developing in the base module
//
void
checkInstantiationLimit(FnSymbol* fn) {
  static Map<FnSymbol*,int> instantiationLimitMap;

  // Don't count instantiations on internal modules
  // nor ones explicitly marked NO_INSTANTIATION_LIMIT.
  if (fn->getModule() &&
      fn->getModule()->modTag != MOD_INTERNAL &&
      !fn->hasFlag(FLAG_NO_INSTANTIATION_LIMIT)) {
    if (instantiationLimitMap.get(fn) >= instantiation_limit) {
      if (fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)) {
        USR_FATAL_CONT(fn->retType, "Type '%s' has been instantiated too many times",
                       fn->retType->symbol->name);
      } else {
        USR_FATAL_CONT(fn, "Function '%s' has been instantiated too many times",
                       fn->name);
      }
      USR_PRINT("  If this is intentional, try increasing"
                " the instantiation limit from %d", instantiation_limit);
      USR_STOP();
    }
    instantiationLimitMap.put(fn, instantiationLimitMap.get(fn)+1);
  }
}


void renameInstantiatedTypeString(TypeSymbol* sym, VarSymbol* var)
{
  const size_t bufSize = 128;
  char immediate[bufSize];
  snprint_imm(immediate, bufSize, *var->immediate);

  // escape quote characters in name string
  char name[bufSize];
  char * name_p = &name[0];
  char * immediate_p = &immediate[0];
  for ( ;
        name_p < &name[bufSize-1] && // don't overflow buffer
          '\0' != *immediate_p;      // stop at null in source
        name_p++, immediate_p++) {
    if ('"' == *immediate_p) { // escape quotes
      *name_p++ = '\\';
    }
    *name_p = *immediate_p;
  }
  *name_p = '\0';
  sym->name = astr(sym->name, name);

  // add ellipsis if too long for buffer
  if (name_p == &name[bufSize-1]) {
    sym->name = astr(sym->name, "...");
  }

  // filter unacceptable characters for cname string
  char cname[bufSize];
  char * cname_p = &cname[0];
  immediate_p = &immediate[0];
  size_t maxNameLength = 32; // add "_etc" after this many characters

  for ( ; immediate_p < &immediate_p[bufSize-1] &&  // don't overflow buffer
          cname_p < &cname[maxNameLength-1] &&      // stop at max length
          '\0' != *immediate_p;
        immediate_p++ ) {
    if (('A' <= *immediate_p && *immediate_p <= 'Z') ||
        ('a' <= *immediate_p && *immediate_p <= 'z') ||
        ('0' <= *immediate_p && *immediate_p <= '9') ||
        ('_' == *immediate_p)) {
      *cname_p = *immediate_p;
      cname_p++;
    }
  }
  *cname_p = '\0';
  sym->cname = astr(sym->cname, cname);

  // add _etc if too long
  if (immediate_p == &immediate[bufSize-1] || // too long for buffer
      cname_p == &cname[maxNameLength-1]) {   // exceeds max length
    sym->cname = astr(sym->cname, "_etc");
  }
}

static void
renameInstantiatedType(TypeSymbol* sym, SymbolMap& subs, FnSymbol* fn) {
  if (sym->name[strlen(sym->name)-1] == ')') {
    // avoid "strange" instantiated type names based on partial instantiation
    //  instead of C(int,real)(imag) this results in C(int,real,imag)
    char* buf = (char*)malloc(strlen(sym->name) + 1);
    memcpy(buf, sym->name, strlen(sym->name));
    buf[strlen(sym->name)-1] = '\0';
    sym->name = astr(buf, ",");
    free(buf);
  } else {
    sym->name = astr(sym->name, "(");
  }
  sym->cname = astr(sym->cname, "_");
  bool first = false;
  for_formals(formal, fn) {
    if (Symbol* value = subs.get(formal)) {
      if (TypeSymbol* ts = toTypeSymbol(value)) {
        if (!first && sym->hasFlag(FLAG_TUPLE)) {
          if (sym->hasFlag(FLAG_STAR_TUPLE)) {
            sym->name = astr(istr(fn->numFormals()-1), "*", ts->name);
            sym->cname = astr(sym->cname, "star_", ts->cname);
            return;
          } else {
            sym->name = astr("(");
          }
        }
        if (!sym->hasFlag(FLAG_STAR_TUPLE)) {
          if (first) {
            sym->name = astr(sym->name, ",");
            sym->cname = astr(sym->cname, "_");
          }
          sym->name = astr(sym->name, ts->name);
          sym->cname = astr(sym->cname, ts->cname);
        }
        first = true;
      } else {
        if (first) {
          sym->name = astr(sym->name, ",");
          sym->cname = astr(sym->cname, "_");
        }
        VarSymbol* var = toVarSymbol(value);
        if (var && var->immediate) {
          Immediate* immediate = var->immediate;
          if (var->type == dtString || var->type == dtStringC)
            renameInstantiatedTypeString(sym, var);
          else if (immediate->const_kind == NUM_KIND_BOOL) {
            // Handle boolean types specially.
            const char* name4bool = immediate->bool_value() ? "true" : "false";
            const char* cname4bool = immediate->bool_value() ? "T" : "F";
            sym->name = astr(sym->name, name4bool);
            sym->cname = astr(sym->cname, cname4bool);
          } else {
            const size_t bufSize = 128;
            char imm[bufSize];
            snprint_imm(imm, bufSize, *var->immediate);
            sym->name = astr(sym->name, imm);
            sym->cname = astr(sym->cname, imm);
          }
        } else {
          sym->name = astr(sym->name, value->cname);
          sym->cname = astr(sym->cname, value->cname);
        }
        first = true;
      }
    }
  }
  sym->name = astr(sym->name, ")");
}

/** Instantiate a type
 *
 * \param fn   Type constructor we are working on
 * \param subs Type substitutions to be made during instantiation
 * \param call The call that is being resolved (used for scope)
 * \param type The generic type we wish to instantiate
 */
static Type*
instantiateTypeForTypeConstructor(FnSymbol* fn, SymbolMap& subs, CallExpr* call, Type* type) {
  INT_ASSERT(isAggregateType(type));
  AggregateType* ct = toAggregateType(type);

  Type* newType = NULL;
  newType = ct->symbol->copy()->type;

  Type *oldParentTy = NULL;
  Type* newParentTy = NULL;
  AggregateType* newCt = toAggregateType(newType);

  // Get the right super type if we are using a super constructor.
  // This only matters for generic parent types.
  if (ct->dispatchParents.n > 0) {
    if(AggregateType *parentTy = toAggregateType(ct->dispatchParents.v[0])){
      if (parentTy->symbol->hasFlag(FLAG_GENERIC)) {
        // Set the type of super to be the instantiated
        // parent with substitutions.

        CallExpr* parentTyCall = new CallExpr(astr("_type_construct_", parentTy->symbol->name));
        // Pass the special formals to the superclass type constructor.
        for_formals(arg, fn) {
          if (arg->hasFlag(FLAG_PARENT_FIELD)) {
            Symbol* value = subs.get(arg);
            if (!value) {
              value = arg;
              // Or error?
            }
            parentTyCall->insertAtTail(value);
          }
        }
        call->insertBefore(parentTyCall);
        resolveCallAndCallee(parentTyCall);

        oldParentTy = parentTy;
        newParentTy = parentTyCall->isResolved()->retType;
        parentTyCall->remove();

        // Now adjust the super field's type.

        DefExpr* superDef = NULL;

        // Find the super field
        for_alist(tmp, newCt->fields) {
          DefExpr* def = toDefExpr(tmp);
          INT_ASSERT(def);
          if (VarSymbol* field = toVarSymbol(def->sym)) {
            if (field->hasFlag(FLAG_SUPER_CLASS)) {
              superDef = def;
            }
          }
        }

        if (superDef) {
          superDef->sym->type = newParentTy;
          INT_ASSERT(newCt->getField("super")->typeInfo() == newParentTy);
        }

      }
    }
  }

  renameInstantiatedType(newType->symbol, subs, fn);

  fn->retType->symbol->defPoint->insertBefore(new DefExpr(newType->symbol));

  newType->symbol->copyFlags(fn);

  if (isSyncType(newType) || isSingleType(newType))
    newType->defaultValue = NULL;

  newType->substitutions.copy(fn->retType->substitutions);

  // Add dispatch parents, but replace parent type with
  // instantiated parent type.
  forv_Vec(Type, t, fn->retType->dispatchParents) {
    Type *useT = t;

    if (t == oldParentTy)
      useT = newParentTy;

    newType->dispatchParents.add(useT);
  }

  forv_Vec(Type, t, fn->retType->dispatchParents) {
    Type *useT = t;

    if (t == oldParentTy)
      useT = newParentTy;

    bool inserted = useT->dispatchChildren.add_exclusive(newType);

    INT_ASSERT(inserted);
  }

  if (newType->dispatchChildren.n)
    INT_FATAL(fn, "generic type has subtypes");

  newType->instantiatedFrom = fn->retType;
  newType->substitutions.map_union(subs);
  newType->symbol->removeFlag(FLAG_GENERIC);

  return newType;
}

/** Instantiate enough of the function for it to make it through the candidate
 *  filtering and disambiguation process.
 *
 * \param fn   Generic function to instantiate
 * \param subs Type substitutions to be made during instantiation
 * \param call Call that is being resolved
 */
FnSymbol* instantiateSignature(FnSymbol*  fn,
                               SymbolMap& subs,
                               CallExpr*  call) {

  //
  // Handle tuples explicitly
  // (_build_tuple, tuple type constructor, tuple default constructor)
  //
  if (FnSymbol* tupleFn = createTupleSignature(fn, subs, call)) {
    return tupleFn;
  }

  form_Map(SymbolMapElem, e, subs) {
    if (TypeSymbol* ts = toTypeSymbol(e->value)) {
      if (ts->type->symbol->hasFlag(FLAG_GENERIC)) {
        INT_FATAL(fn, "illegal instantiation with a generic type");
      }

      TypeSymbol* nts = getNewSubType(fn, e->key, ts);

      if (ts != nts) {
        e->value = nts;
      }
    }
  }

  //
  // determine root function in the case of partial instantiation
  //
  FnSymbol* root = fn;

  while (root->instantiatedFrom != NULL &&
         root->numFormals()     == root->instantiatedFrom->numFormals()) {
    root = root->instantiatedFrom;
  }

  //
  // determine all substitutions (past substitutions in a partial
  // instantiation plus the current substitutions) and change the
  // substitutions to refer to the root function's formal arguments
  //
  SymbolMap all_subs;

  if (fn->instantiatedFrom) {
    form_Map(SymbolMapElem, e, fn->substitutions) {
      all_subs.put(e->key, e->value);
    }
  }

  form_Map(SymbolMapElem, e, subs) {
    copyGenericSub(all_subs, root, fn, e->key, e->value);
  }

  //
  // use cached instantiation if possible
  //
  if (FnSymbol* cached = checkCache(genericsCache, root, &all_subs)) {
    if (cached != (FnSymbol*)gVoid) {
      checkInfiniteWhereInstantiation(cached);

      return cached;
    } else {
      return NULL;
    }
  }

  SET_LINENO(fn);

  //
  // copy generic class type if this function is a type constructor
  //
  Type* newType = NULL;

  if (fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)) {
    newType = instantiateTypeForTypeConstructor(fn, subs, call, fn->retType);
  }

  //
  // instantiate function
  //

  SymbolMap map;

  if (newType) {
    map.put(fn->retType->symbol, newType->symbol);
  }

  FnSymbol* newFn = fn->partialCopy(&map);

  addCache(genericsCache, root, newFn, &all_subs);

  newFn->removeFlag(FLAG_GENERIC);
  newFn->addFlag(FLAG_INVISIBLE_FN);
  newFn->instantiatedFrom = fn;
  newFn->substitutions.map_union(all_subs);

  if (call) {
    newFn->instantiationPoint = getVisibilityBlock(call);
  }

  Expr* putBefore = fn->defPoint;

  if (putBefore->list == NULL) {
    putBefore = call->parentSymbol->defPoint;
  }

  putBefore->insertBefore(new DefExpr(newFn));

  //
  // add parameter instantiations to parameter map
  //
  for (int i = 0; i < subs.n; i++) {
    if (ArgSymbol* arg = toArgSymbol(subs.v[i].key)) {
      if (arg->intent == INTENT_PARAM) {
        Symbol* key = map.get(arg);
        Symbol* val = subs.v[i].value;

        if (!key || !val || isTypeSymbol(val)) {
          INT_FATAL("error building parameter map in instantiation");
        }

        paramMap.put(key, val);
      }
    }
  }

  //
  // extend parameter map if parameter intent argument is instantiated
  // again; this may happen because the type is omitted and the
  // argument is later instantiated based on the type of the parameter
  //
  for_formals(arg, fn) {
    if (paramMap.get(arg)) {
      Symbol* key = map.get(arg);
      Symbol* val = paramMap.get(arg);

      if (!key || !val) {
        INT_FATAL("error building parameter map in instantiation");
      }

      paramMap.put(key, val);
    }
  }

  //
  // set types and attributes of instantiated function's formals; also
  // set up a defaultExpr for the new formal (why is this done?)
  //
  for_formals(formal, fn) {
    ArgSymbol* newFormal = toArgSymbol(map.get(formal));

    if (Symbol* value = subs.get(formal)) {
      INT_ASSERT(formal->intent == INTENT_PARAM || isTypeSymbol(value));

      if (formal->intent == INTENT_PARAM) {
        newFormal->intent = INTENT_BLANK;

        newFormal->addFlag(FLAG_INSTANTIATED_PARAM);

        if (newFormal->type->symbol->hasFlag(FLAG_GENERIC)) {
          newFormal->type = paramMap.get(newFormal)->type;
        }

      } else {
        newFormal->instantiatedFrom = formal->type;
        newFormal->type             = value->type;
      }

      if (!newFormal->defaultExpr || formal->hasFlag(FLAG_TYPE_VARIABLE)) {
        Symbol* defaultSym = NULL;

        if (newFormal->defaultExpr) {
          newFormal->defaultExpr->remove();
        }

        if (Symbol* sym = paramMap.get(newFormal)) {
          defaultSym = sym;
        } else {
          defaultSym = gTypeDefaultToken;
        }

        newFormal->defaultExpr = new BlockStmt(new SymExpr(defaultSym));

        insert_help(newFormal->defaultExpr, NULL, newFormal);
      }
    }
  }

  if (newType) {
    newType->defaultTypeConstructor = newFn;
    newFn->retType                  = newType;
  }
  
  bool fixedTuple = fixupTupleFunctions(fn, newFn, call);
  // Fix up chpl__initCopy for user-defined records
  if (!fixedTuple &&
      fn->hasFlag(FLAG_INIT_COPY_FN) &&
      fn->hasFlag(FLAG_COMPILER_GENERATED) ) {
    // Generate the initCopy function based upon initializer
    fixupDefaultInitCopy(fn, newFn, call);
  }

  if (newFn->numFormals()       >  1 &&
      newFn->getFormal(1)->type == dtMethodToken) {
    newFn->getFormal(2)->type->methods.add(newFn);
  }

  newFn->tagIfGeneric();

  //
  // TODO: What would it take to remove this evaluation of the where
  // clause along this generic path and only resolve it on the
  // concrete path once we have eliminated candidates based on
  // actual-formal matches?  Simply removing it doesn't work at
  // present.
  //
  if (newFn->hasFlag(FLAG_GENERIC) == false &&
      evaluateWhereClause(newFn)   == false) {
    //
    // where clause evaluates to false so cache gVoid as a function
    //
    replaceCache(genericsCache, root, (FnSymbol*)gVoid, &all_subs);

    return NULL;
  }

  if (explainInstantiationLine == -2) {
    parseExplainFlag(fExplainInstantiation,
                     &explainInstantiationLine,
                     &explainInstantiationModule);
  }

  if (!newFn->hasFlag(FLAG_GENERIC) && explainInstantiationLine) {
    explainInstantiation(newFn);
  }

  checkInstantiationLimit(fn);

  return newFn;
}


bool evaluateWhereClause(FnSymbol* fn, bool generic) {
  if (fn->where) {
    whereStack.add(fn);

    resolveFormals(fn);

    resolveBlockStmt(fn->where);

    whereStack.pop();

    SymExpr* se = toSymExpr(fn->where->body.last());

    if (se == NULL) {
      //
      // if we're evaluating the where clause of a generic function,
      // it's too soon to throw errors because we haven't yet
      // determined whether the call is even a candidate based on
      // actual-formal matching.  For that reason, conservatively
      // return 'true' in error cases.  We'll then re-evaluate the
      // where clause on the concrete instantiation of the generic
      // function and issue the error (if appropriate) there.
      //
      if (generic) {
        return true;
      } else {
        USR_FATAL(fn->where, "invalid where clause");
      }
    }

    if (se->symbol() == gFalse) {
      return false;
    }

    if (se->symbol() != gTrue) {
      USR_FATAL(fn->where, "invalid where clause");
    }
  }

  return true;
}














/** Finish copying and instantiating the partially instantiated function.
 *
 * TODO: See if more code from instantiateSignature can be moved into this
 *       function.
 *
 * \param fn   Generic function to finish instantiating
 */
void
instantiateBody(FnSymbol* fn) {
  if (getPartialCopyInfo(fn)) {
    fn->finalizeCopy();
  }
}

/** Fully instantiate a generic function given a map of substitutions and a
 *  call site.
 *
 * \param fn   Generic function to instantiate
 * \param subs Type substitutions to be made during instantiation
 * \param call Call that is being resolved
 */
FnSymbol*
instantiate(FnSymbol* fn, SymbolMap& subs, CallExpr* call) {
  FnSymbol* newFn;

  newFn = instantiateSignature(fn, subs, call);

  if (newFn != NULL) {
    instantiateBody(newFn);
  }

  return newFn;
}
