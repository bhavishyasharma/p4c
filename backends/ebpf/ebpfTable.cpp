/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "ebpfTable.h"
#include "ebpfType.h"
#include "ir/ir.h"
#include "frontends/p4/coreLibrary.h"
#include "frontends/p4/methodInstance.h"

namespace EBPF {

bool ActionTranslationVisitor::preorder(const IR::PathExpression* expression) {
    if (isActionParameter(expression)) {
        cstring paramStr = getActionParamStr(expression);
        builder->append(paramStr.c_str());
        return false;
    }
    visit(expression->path);
    return false;
}

bool ActionTranslationVisitor::isActionParameter(const IR::PathExpression *expression) const {
    auto decl = program->refMap->getDeclaration(expression->path, true);
    if (decl->is<IR::Parameter>()) {
        auto param = decl->to<IR::Parameter>();
        return action->parameters->getParameter(param->name) == param;
    }
    return false;
}

cstring ActionTranslationVisitor::getActionParamStr(const IR::Expression *expression) const {
    cstring actionName = EBPFObject::externalName(action);
    auto paramStr = Util::printf_format("%s->u.%s.%s",
                                        valueName, actionName,
                                        expression->toString());
    return paramStr;
}

bool ActionTranslationVisitor::preorder(const IR::P4Action* act) {
    action = act;
    visit(action->body);
    return false;
}

////////////////////////////////////////////////////////////////

EBPFTable::EBPFTable(const EBPFProgram* program, const IR::TableBlock* table,
                     CodeGenInspector* codeGen) :
        EBPFTableBase(program, EBPFObject::externalName(table->container), codeGen), table(table) {
    cstring base = instanceName + "_defaultAction";
    defaultActionMapName = base;

    base = table->container->name.name + "_actions";
    actionEnumName = program->refMap->newName(base);

    keyGenerator = table->container->getKey();
    actionList = table->container->getActionList();

    initKey();
}

void EBPFTable::initKey() {
    if (keyGenerator != nullptr) {
        unsigned fieldNumber = 0;
        for (auto c : keyGenerator->keyElements) {
            auto type = program->typeMap->getType(c->expression);
            auto ebpfType = EBPFTypeFactory::instance->create(type);
            if (!ebpfType->is<IHasWidth>()) {
                ::error(ErrorType::ERR_TYPE_ERROR,
                        "%1%: illegal type %2% for key field", c, type);
                return;
            }

            cstring fieldName = cstring("field") + Util::toString(fieldNumber);
            keyTypes.emplace(c, ebpfType);
            keyFieldNames.emplace(c, fieldName);
            fieldNumber++;
        }
    }
}

// Performs the following validations:
// 1. Validates if LPM key is the last one from match keys (ignores selector fields).
void EBPFTable::validateKeys() const {
    if (keyGenerator == nullptr)
        return;

    auto lastKey = std::find_if(
            keyGenerator->keyElements.rbegin(), keyGenerator->keyElements.rend(),
            [](const IR::KeyElement * key)
                { return key->matchType->path->name.name != "selector"; });

    for (auto it : keyGenerator->keyElements) {
        auto mtdecl = program->refMap->getDeclaration(it->matchType->path, true);
        auto matchType = mtdecl->getNode()->to<IR::Declaration_ID>();
        if (matchType->name.name == P4::P4CoreLibrary::instance.lpmMatch.name) {
            if (it != *lastKey) {
                ::error(ErrorType::ERR_UNSUPPORTED,
                        "%1% field key must be at the end of whole key", it->matchType);
            }
        }
    }
}

void EBPFTable::emitKeyType(CodeBuilder* builder) {
    builder->emitIndent();
    builder->appendFormat("struct %s ", keyTypeName.c_str());
    builder->blockStart();

    CodeGenInspector commentGen(program->refMap, program->typeMap);
    commentGen.setBuilder(builder);

    if (keyGenerator != nullptr) {
        if (isLPMTable()) {
            // For LPM kind key we need an additional 32 bit field - prefixlen
            auto prefixType = EBPFTypeFactory::instance->create(IR::Type_Bits::get(32));
            builder->emitIndent();
            prefixType->declare(builder, prefixFieldName, false);
            builder->endOfStatement(true);
        }

        for (auto c : keyGenerator->keyElements) {
            auto mtdecl = program->refMap->getDeclaration(c->matchType->path, true);
            auto matchType = mtdecl->getNode()->to<IR::Declaration_ID>();

            auto ebpfType = ::get(keyTypes, c);
            cstring fieldName = ::get(keyFieldNames, c);

            if (!isMatchTypeSupported(matchType)) {
                ::error(ErrorType::ERR_UNSUPPORTED,
                        "Match of type %1% not supported", c->matchType);
            }

            builder->emitIndent();
            ebpfType->declare(builder, fieldName, false);
            builder->append("; /* ");
            c->expression->apply(commentGen);
            builder->append(" */");
            builder->newline();
        }
    }

    // Add dummy key if P4 table define table with empty key. This due to that hash map
    // cannot have zero-length key. See function htab_map_alloc_check in kernel/bpf/hashtab.c
    // located in Linux kernel repository
    if (keyFieldNames.empty()) {
        builder->emitIndent();
        builder->appendLine("u8 __dummy_table_key;");
    }

    builder->blockEnd(false);
    builder->append(" __attribute__((aligned(4)))");
    builder->endOfStatement(true);
}

void EBPFTable::emitActionArguments(CodeBuilder* builder,
                                    const IR::P4Action* action, cstring name) {
    builder->emitIndent();
    builder->append("struct ");
    builder->blockStart();

    for (auto p : *action->parameters->getEnumerator()) {
        builder->emitIndent();
        auto type = EBPFTypeFactory::instance->create(p->type);
        type->declare(builder, p->externalName(), false);
        builder->endOfStatement(true);
    }

    builder->blockEnd(false);
    builder->spc();
    builder->append(name);
    builder->endOfStatement(true);
}

void EBPFTable::emitValueType(CodeBuilder* builder) {
    emitValueActionIDNames(builder);

    // a type-safe union: a struct with a tag and an union
    builder->emitIndent();
    builder->appendFormat("struct %s ", valueTypeName.c_str());
    builder->blockStart();

    emitValueStructStructure(builder);

    builder->blockEnd(false);
    builder->endOfStatement(true);
}

void EBPFTable::emitValueActionIDNames(CodeBuilder* builder) {
    // create type definition for action
    builder->emitIndent();
    unsigned int action_idx = 1;  // 0 is reserved for NoAction
    for (auto a : actionList->actionList) {
        auto adecl = program->refMap->getDeclaration(a->getPath(), true);
        auto action = adecl->getNode()->to<IR::P4Action>();
        // no need to define a constant for NoAction,
        // "case 0" will be explicitly generated in the action handling switch
        if (action->name.originalName == P4::P4CoreLibrary::instance.noAction.name) {
            continue;
        }
        builder->emitIndent();
        builder->appendFormat("#define %s %d", p4ActionToActionIDName(action), action_idx);
        builder->newline();
        action_idx++;
    }
    builder->emitIndent();
}

void EBPFTable::emitValueStructStructure(CodeBuilder* builder) {
    builder->emitIndent();
    builder->append("unsigned int action;");
    builder->newline();

    builder->emitIndent();
    builder->append("union ");
    builder->blockStart();

    // Declare NoAction data structure at the beginning as it has reserved id 0
    builder->emitIndent();
    builder->appendLine("struct {");
    builder->emitIndent();
    builder->append("} _NoAction");
    builder->endOfStatement(true);

    for (auto a : actionList->actionList) {
        auto adecl = program->refMap->getDeclaration(a->getPath(), true);
        auto action = adecl->getNode()->to<IR::P4Action>();
        if (action->name.originalName == P4::P4CoreLibrary::instance.noAction.name)
            continue;
        cstring name = EBPFObject::externalName(action);
        emitActionArguments(builder, action, name);
    }

    builder->blockEnd(false);
    builder->spc();
    builder->appendLine("u;");
}

void EBPFTable::emitTypes(CodeBuilder* builder) {
    validateKeys();
    emitKeyType(builder);
    emitValueType(builder);
}

void EBPFTable::emitInstance(CodeBuilder* builder) {
    if (keyGenerator != nullptr) {
        auto impl = table->container->properties->getProperty(
            program->model.tableImplProperty.name);
        if (impl == nullptr) {
            ::error(ErrorType::ERR_EXPECTED, "Table %1% does not have an %2% property",
                    table->container, program->model.tableImplProperty.name);
            return;
        }

        // Some type checking...
        if (!impl->value->is<IR::ExpressionValue>()) {
            ::error(ErrorType::ERR_EXPECTED,
                    "%1%: Expected property to be an `extern` block", impl);
            return;
        }

        auto expr = impl->value->to<IR::ExpressionValue>()->expression;
        if (!expr->is<IR::ConstructorCallExpression>()) {
            ::error(ErrorType::ERR_EXPECTED,
                    "%1%: Expected property to be an `extern` block", impl);
            return;
        }

        auto block = table->getValue(expr);
        if (block == nullptr || !block->is<IR::ExternBlock>()) {
            ::error(ErrorType::ERR_EXPECTED,
                    "%1%: Expected property to be an `extern` block", impl);
            return;
        }

        TableKind tableKind;
        auto extBlock = block->to<IR::ExternBlock>();
        if (extBlock->type->name.name == program->model.array_table.name) {
            tableKind = TableArray;
        } else if (extBlock->type->name.name == program->model.hash_table.name) {
            tableKind = TableHash;
        } else {
            ::error(ErrorType::ERR_EXPECTED,
                    "%1%: implementation must be one of %2% or %3%",
                    impl, program->model.array_table.name, program->model.hash_table.name);
            return;
        }

        // If any key field is LPM we will generate an LPM table
        for (auto it : keyGenerator->keyElements) {
            auto mtdecl = program->refMap->getDeclaration(it->matchType->path, true);
            auto matchType = mtdecl->getNode()->to<IR::Declaration_ID>();
            if (matchType->name.name == P4::P4CoreLibrary::instance.lpmMatch.name) {
                if (tableKind == TableLPMTrie) {
                    ::error(ErrorType::ERR_UNSUPPORTED,
                            "%1%: only one LPM field allowed", it->matchType);
                    return;
                }
                tableKind = TableLPMTrie;
            }
        }

        auto sz = extBlock->getParameterValue(program->model.array_table.size.name);
        if (sz == nullptr || !sz->is<IR::Constant>()) {
            ::error(ErrorType::ERR_UNSUPPORTED,
                    "%1%: Expected an integer argument; is the model corrupted?", expr);
            return;
        }
        auto cst = sz->to<IR::Constant>();
        if (!cst->fitsInt()) {
            ::error(ErrorType::ERR_UNSUPPORTED, "%1%: size too large", cst);
            return;
        }
        int size = cst->asInt();
        if (size <= 0) {
            ::error(ErrorType::ERR_INVALID, "%1%: negative size", cst);
            return;
        }

        cstring name = EBPFObject::externalName(table->container);
        builder->target->emitTableDecl(builder, name, tableKind,
                                       cstring("struct ") + keyTypeName,
                                       cstring("struct ") + valueTypeName, size);
    }
    builder->target->emitTableDecl(builder, defaultActionMapName, TableArray,
                                   program->arrayIndexType,
                                   cstring("struct ") + valueTypeName, 1);
}

void EBPFTable::emitKey(CodeBuilder* builder, cstring keyName) {
    if (keyGenerator == nullptr) {
        return;
    }

    if (isLPMTable()) {
        builder->emitIndent();
        builder->appendFormat("%s.%s = sizeof(%s)*8 - %d",
                              keyName.c_str(), prefixFieldName,
                              keyName, prefixLenFieldWidth);
        builder->endOfStatement(true);
    }

    for (auto c : keyGenerator->keyElements) {
        auto ebpfType = ::get(keyTypes, c);
        cstring fieldName = ::get(keyFieldNames, c);
        if (fieldName == nullptr || ebpfType == nullptr)
            continue;
        bool memcpy = false;
        EBPFScalarType* scalar = nullptr;
        cstring swap;
        if (ebpfType->is<EBPFScalarType>()) {
            scalar = ebpfType->to<EBPFScalarType>();
            unsigned width = scalar->implementationWidthInBits();
            memcpy = !EBPFScalarType::generatesScalar(width);

            if (width <= 8) {
                swap = "";  // single byte, nothing to swap
            } else if (width <= 16) {
                swap = "bpf_htons";
            } else if (width <= 32) {
                swap = "bpf_htonl";
            } else if (width <= 64) {
                swap = "bpf_htonll";
            } else {
                // TODO: handle width > 64 bits
                ::error(ErrorType::ERR_UNSUPPORTED,
                        "%1%: fields wider than 64 bits are not supported yet",
                        fieldName);
            }
        }

        bool isLPMKeyBigEndian = false;
        if (isLPMTable()) {
            if (c->matchType->path->name.name == P4::P4CoreLibrary::instance.lpmMatch.name)
                isLPMKeyBigEndian = true;
        }

        builder->emitIndent();
        if (memcpy) {
            if (isLPMKeyBigEndian) {
                // FIXME: will not work on big endian machines because byte swap
                //  is done always. Also test this solution because fields larger
                //  than 64 bit are not deparsed correctly
                const unsigned bytesToCopy = scalar->bytesRequired();
                for (unsigned byte = 0; byte < bytesToCopy; ++byte) {
                    builder->appendFormat("%s.%s[%u] = (",
                                          keyName.c_str(), fieldName.c_str(), byte);
                    codeGen->visit(c->expression);
                    builder->appendFormat(")[%u]", bytesToCopy - byte - 1);
                    builder->endOfStatement(true);
                    builder->emitIndent();
                }
            } else {
                builder->appendFormat("memcpy(&%s.%s, &", keyName.c_str(), fieldName.c_str());
                codeGen->visit(c->expression);
                builder->appendFormat(", %d)", scalar->bytesRequired());
            }
        } else {
            builder->appendFormat("%s.%s = ", keyName.c_str(), fieldName.c_str());
            if (isLPMKeyBigEndian)
                builder->appendFormat("%s(", swap.c_str());
            codeGen->visit(c->expression);
            if (isLPMKeyBigEndian)
                builder->append(")");
        }
        builder->endOfStatement(true);

        cstring msgStr, varStr;
        if (memcpy) {
            msgStr = Util::printf_format("Control: key %s", c->expression->toString());
            builder->target->emitTraceMessage(builder, msgStr.c_str());
        } else {
            msgStr = Util::printf_format("Control: key %s=0x%%llx", c->expression->toString());
            varStr = Util::printf_format("(unsigned long long) %s.%s",
                                         keyName.c_str(), fieldName.c_str());
            builder->target->emitTraceMessage(builder, msgStr.c_str(), 1, varStr.c_str());
        }
    }
}

void EBPFTable::emitAction(CodeBuilder* builder, cstring valueName, cstring actionRunVariable) {
    builder->emitIndent();
    builder->appendFormat("switch (%s->action) ", valueName.c_str());
    builder->blockStart();

    for (auto a : actionList->actionList) {
        auto adecl = program->refMap->getDeclaration(a->getPath(), true);
        auto action = adecl->getNode()->to<IR::P4Action>();
        cstring name = EBPFObject::externalName(action), msgStr, convStr;
        builder->emitIndent();
        cstring actionName = p4ActionToActionIDName(action);
        builder->appendFormat("case %s: ", actionName);
        builder->newline();
        builder->increaseIndent();

        msgStr = Util::printf_format("Control: executing action %s", name);
        builder->target->emitTraceMessage(builder, msgStr.c_str());
        for (auto param : *(action->parameters)) {
            auto etype = EBPFTypeFactory::instance->create(param->type);
            unsigned width = dynamic_cast<IHasWidth*>(etype)->widthInBits();

            if (width <= 64) {
                convStr = Util::printf_format("(unsigned long long) (%s->u.%s.%s)",
                                              valueName, name, param->toString());
                msgStr = Util::printf_format("Control: param %s=0x%%llx (%d bits)",
                                             param->toString(), width);
                builder->target->emitTraceMessage(builder, msgStr.c_str(), 1, convStr.c_str());
            } else {
                msgStr = Util::printf_format("Control: param %s (%d bits)",
                                             param->toString(), width);
                builder->target->emitTraceMessage(builder, msgStr.c_str());
            }
        }

        builder->emitIndent();

        auto visitor = createActionTranslationVisitor(valueName, program);
        visitor->setBuilder(builder);
        visitor->copySubstitutions(codeGen);

        action->apply(*visitor);
        builder->newline();
        builder->emitIndent();
        builder->appendLine("break;");
        builder->decreaseIndent();
    }

    builder->emitIndent();
    builder->appendLine("default:");
    builder->increaseIndent();
    builder->target->emitTraceMessage(builder, "Control: Invalid action type, aborting");

    builder->emitIndent();
    builder->appendFormat("return %s", builder->target->abortReturnCode().c_str());
    builder->endOfStatement(true);
    builder->decreaseIndent();

    builder->blockEnd(true);

    if (!actionRunVariable.isNullOrEmpty()) {
        builder->emitIndent();
        builder->appendFormat("%s = %s->action",
                              actionRunVariable.c_str(), valueName.c_str());
        builder->endOfStatement(true);
    }
}

void EBPFTable::emitInitializer(CodeBuilder* builder) {
    // emit code to initialize the default action
    const IR::P4Table* t = table->container;
    const IR::Expression* defaultAction = t->getDefaultAction();
    BUG_CHECK(defaultAction->is<IR::MethodCallExpression>(),
              "%1%: expected an action call", defaultAction);
    auto mce = defaultAction->to<IR::MethodCallExpression>();
    auto mi = P4::MethodInstance::resolve(mce, program->refMap, program->typeMap);

    auto ac = mi->to<P4::ActionCall>();
    BUG_CHECK(ac != nullptr, "%1%: expected an action call", mce);
    auto action = ac->action;
    cstring name = EBPFObject::externalName(action);
    cstring fd = "tableFileDescriptor";
    cstring defaultTable = defaultActionMapName;
    cstring value = "value";
    cstring key = "key";

    builder->emitIndent();
    builder->blockStart();
    builder->emitIndent();
    builder->appendFormat("int %s = BPF_OBJ_GET(MAP_PATH \"/%s\")",
                          fd.c_str(), defaultTable.c_str());
    builder->endOfStatement(true);
    builder->emitIndent();
    builder->appendFormat("if (%s < 0) { fprintf(stderr, \"map %s not loaded\\n\"); exit(1); }",
                          fd.c_str(), defaultTable.c_str());
    builder->newline();

    builder->emitIndent();
    builder->appendFormat("struct %s %s = ", valueTypeName.c_str(), value.c_str());
    builder->blockStart();
    builder->emitIndent();
    cstring actionName = p4ActionToActionIDName(action);
    builder->appendFormat(".action = %s,", actionName);
    builder->newline();

    CodeGenInspector cg(program->refMap, program->typeMap);
    cg.setBuilder(builder);

    builder->emitIndent();
    builder->appendFormat(".u = {.%s = {", name.c_str());
    for (auto p : *mi->substitution.getParametersInArgumentOrder()) {
        auto arg = mi->substitution.lookup(p);
        arg->apply(cg);
        builder->append(",");
    }
    builder->append("}},\n");

    builder->blockEnd(false);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->append("int ok = ");
    builder->target->emitUserTableUpdate(builder, fd, program->zeroKey, value);
    builder->newline();

    builder->emitIndent();
    builder->appendFormat("if (ok != 0) { "
                          "perror(\"Could not write in %s\"); exit(1); }",
                          defaultTable.c_str());
    builder->newline();
    builder->blockEnd(true);

    // Emit code for table initializer
    auto entries = t->getEntries();
    if (entries == nullptr)
        return;

    builder->emitIndent();
    builder->blockStart();
    builder->emitIndent();
    builder->appendFormat("int %s = BPF_OBJ_GET(MAP_PATH \"/%s\")",
                          fd.c_str(), dataMapName.c_str());
    builder->endOfStatement(true);
    builder->emitIndent();
    builder->appendFormat("if (%s < 0) { fprintf(stderr, \"map %s not loaded\\n\"); exit(1); }",
                          fd.c_str(), dataMapName.c_str());
    builder->newline();

    for (auto e : entries->entries) {
        builder->emitIndent();
        builder->blockStart();

        auto entryAction = e->getAction();
        builder->emitIndent();
        builder->appendFormat("struct %s %s = {", keyTypeName.c_str(), key.c_str());
        e->getKeys()->apply(cg);
        builder->append("}");
        builder->endOfStatement(true);

        BUG_CHECK(entryAction->is<IR::MethodCallExpression>(),
                  "%1%: expected an action call", defaultAction);
        auto mce = entryAction->to<IR::MethodCallExpression>();
        auto mi = P4::MethodInstance::resolve(mce, program->refMap, program->typeMap);

        auto ac = mi->to<P4::ActionCall>();
        BUG_CHECK(ac != nullptr, "%1%: expected an action call", mce);
        auto action = ac->action;
        cstring name = EBPFObject::externalName(action);

        builder->emitIndent();
        builder->appendFormat("struct %s %s = ",
                              valueTypeName.c_str(), value.c_str());
        builder->blockStart();
        builder->emitIndent();
        cstring actionName = p4ActionToActionIDName(action);
        builder->appendFormat(".action = %s,", actionName);
        builder->newline();

        CodeGenInspector cg(program->refMap, program->typeMap);
        cg.setBuilder(builder);

        builder->emitIndent();
        builder->appendFormat(".u = {.%s = {", name.c_str());
        for (auto p : *mi->substitution.getParametersInArgumentOrder()) {
            auto arg = mi->substitution.lookup(p);
            arg->apply(cg);
            builder->append(",");
        }
        builder->append("}},\n");

        builder->blockEnd(false);
        builder->endOfStatement(true);

        builder->emitIndent();
        builder->append("int ok = ");
        builder->target->emitUserTableUpdate(builder, fd, key, value);
        builder->newline();

        builder->emitIndent();
        builder->appendFormat("if (ok != 0) { "
                              "perror(\"Could not write in %s\"); exit(1); }",
                              t->name.name.c_str());
        builder->newline();
        builder->blockEnd(true);
    }
    builder->blockEnd(true);
}

cstring EBPFTable::p4ActionToActionIDName(const IR::P4Action * action) const {
    if (action->name.originalName == P4::P4CoreLibrary::instance.noAction.name) {
        // NoAction always gets ID=0.
        return "0";
    }

    cstring actionName = EBPFObject::externalName(action);
    cstring tableInstance = dataMapName;
    return Util::printf_format("%s_ACT_%s", tableInstance.toUpper(), actionName.toUpper());
}

// As ternary has precedence over lpm, this function checks if any
// field is key field is lpm and none of key fields is of type ternary.
bool EBPFTable::isLPMTable() {
    bool isLPM = false;
    if (keyGenerator != nullptr) {
        // If any key field is LPM we will generate an LPM table
        for (auto it : keyGenerator->keyElements) {
            auto mtdecl = program->refMap->getDeclaration(it->matchType->path, true);
            auto matchType = mtdecl->getNode()->to<IR::Declaration_ID>();
            if (matchType->name.name == P4::P4CoreLibrary::instance.ternaryMatch.name) {
                // if there is a ternary field, we are sure, it is not a LPM table.
                return false;
            } else if (matchType->name.name == P4::P4CoreLibrary::instance.lpmMatch.name) {
                isLPM = true;
            }
        }
    }

    return isLPM;
}

////////////////////////////////////////////////////////////////

EBPFCounterTable::EBPFCounterTable(const EBPFProgram* program, const IR::ExternBlock* block,
                                   cstring name, CodeGenInspector* codeGen) :
        EBPFTableBase(program, name, codeGen) {
    auto sz = block->getParameterValue(program->model.counterArray.max_index.name);
    if (sz == nullptr || !sz->is<IR::Constant>()) {
        ::error(ErrorType::ERR_INVALID,
                "%1% (%2%): expected an integer argument; is the model corrupted?",
                program->model.counterArray.max_index, name);
        return;
    }
    auto cst = sz->to<IR::Constant>();
    if (!cst->fitsInt()) {
        ::error(ErrorType::ERR_OVERLIMIT, "%1%: size too large", cst);
        return;
    }
    size = cst->asInt();
    if (size <= 0) {
        ::error(ErrorType::ERR_OVERLIMIT, "%1%: negative size", cst);
        return;
    }

    auto sprs = block->getParameterValue(program->model.counterArray.sparse.name);
    if (sprs == nullptr || !sprs->is<IR::BoolLiteral>()) {
        ::error(ErrorType::ERR_INVALID,
                "%1% (%2%): Expected an integer argument; is the model corrupted?",
                program->model.counterArray.sparse, name);
        return;
    }

    isHash = sprs->to<IR::BoolLiteral>()->value;
}

void EBPFCounterTable::emitInstance(CodeBuilder* builder) {
    TableKind kind = isHash ? TableHash : TableArray;
    builder->target->emitTableDecl(
        builder, dataMapName, kind, keyTypeName, valueTypeName, size);
}

void EBPFCounterTable::emitCounterIncrement(CodeBuilder* builder,
                                            const IR::MethodCallExpression *expression) {
    cstring keyName = program->refMap->newName("key");
    cstring valueName = program->refMap->newName("value");

    builder->emitIndent();
    builder->append(valueTypeName);
    builder->spc();
    builder->append("*");
    builder->append(valueName);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->append(valueTypeName);
    builder->spc();
    builder->appendLine("init_val = 1;");

    builder->emitIndent();
    builder->append(keyTypeName);
    builder->spc();
    builder->append(keyName);
    builder->append(" = ");

    BUG_CHECK(expression->arguments->size() == 1, "Expected just 1 argument for %1%", expression);
    auto arg = expression->arguments->at(0);

    codeGen->visit(arg);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->target->emitTableLookup(builder, dataMapName, keyName, valueName);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->appendFormat("if (%s != NULL)", valueName.c_str());
    builder->newline();
    builder->increaseIndent();
    builder->emitIndent();
    builder->appendFormat("__sync_fetch_and_add(%s, 1);", valueName.c_str());
    builder->newline();
    builder->decreaseIndent();

    builder->emitIndent();
    builder->appendLine("else");
    builder->increaseIndent();
    builder->emitIndent();
    builder->target->emitTableUpdate(builder, dataMapName, keyName, "init_val");
    builder->newline();
    builder->decreaseIndent();
}

void EBPFCounterTable::emitCounterAdd(CodeBuilder* builder,
                                            const IR::MethodCallExpression *expression) {
    cstring keyName = program->refMap->newName("key");
    cstring valueName = program->refMap->newName("value");
    cstring incName = program->refMap->newName("inc");

    builder->emitIndent();
    builder->append(valueTypeName);
    builder->spc();
    builder->append("*");
    builder->append(valueName);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->append(valueTypeName);
    builder->spc();
    builder->appendLine("init_val = 1;");

    builder->emitIndent();
    builder->append(keyTypeName);
    builder->spc();
    builder->append(keyName);
    builder->append(" = ");

    BUG_CHECK(expression->arguments->size() == 2, "Expected just 2 arguments for %1%", expression);
    auto index = expression->arguments->at(0);

    codeGen->visit(index);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->append(valueTypeName);
    builder->spc();
    builder->append(incName);
    builder->append(" = ");

    auto inc = expression->arguments->at(1);

    codeGen->visit(inc);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->target->emitTableLookup(builder, dataMapName, keyName, valueName);
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->appendFormat("if (%s != NULL)", valueName.c_str());
    builder->newline();
    builder->increaseIndent();
    builder->emitIndent();
    builder->appendFormat("__sync_fetch_and_add(%s, %s);", valueName.c_str(), incName.c_str());
    builder->newline();
    builder->decreaseIndent();

    builder->emitIndent();
    builder->appendLine("else");
    builder->increaseIndent();
    builder->emitIndent();
    builder->target->emitTableUpdate(builder, dataMapName, keyName, "init_val");
    builder->newline();
    builder->decreaseIndent();
}

void
EBPFCounterTable::emitMethodInvocation(CodeBuilder* builder, const P4::ExternMethod* method) {
    if (method->method->name.name == program->model.counterArray.increment.name) {
        emitCounterIncrement(builder, method->expr);
        return;
    }
    if (method->method->name.name == program->model.counterArray.add.name) {
        emitCounterAdd(builder, method->expr);
        return;
    }
    ::error(ErrorType::ERR_UNSUPPORTED,
            "Unexpected method %1% for %2%", method->expr, program->model.counterArray.name);
}

void EBPFCounterTable::emitTypes(CodeBuilder* builder) {
    builder->emitIndent();
    builder->appendFormat("typedef %s %s",
                          EBPFModel::instance.counterIndexType.c_str(), keyTypeName.c_str());
    builder->endOfStatement(true);
    builder->emitIndent();
    builder->appendFormat("typedef %s %s",
                          EBPFModel::instance.counterValueType.c_str(), valueTypeName.c_str());
    builder->endOfStatement(true);
}

}  // namespace EBPF
