#include "jsonconverter.h"
#include "lib/gmputil.h"
#include "frontends/p4/coreLibrary.h"
#include "ir/ir.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/p4/enumInstance.h"
#include "analyzer.h"

namespace BMV2 {

DirectMeterMap::DirectMeterInfo* DirectMeterMap::createInfo(const IR::IDeclaration* meter) {
    auto prev = ::get(directMeter, meter);
    BUG_CHECK(prev == nullptr, "Already created");
    auto result = new DirectMeterMap::DirectMeterInfo();
    directMeter.emplace(meter, result);
    return result;
}

DirectMeterMap::DirectMeterInfo* DirectMeterMap::getInfo(const IR::IDeclaration* meter) {
    return ::get(directMeter, meter);
}

void DirectMeterMap::setTable(const IR::IDeclaration* meter, const IR::TableContainer* table) {
    auto info = getInfo(meter);
    CHECK_NULL(info);
    if (info->table != nullptr)
        ::error("%1%: Direct meters cannot be attached to multiple tables %2% and %3%",
                meter, table, info->table);
    info->table = table;
}

static bool checkSame(const IR::Expression* expr0, const IR::Expression* expr1) {
    if (expr0->node_type_name() != expr1->node_type_name())
        return false;
    if (expr0->is<IR::PathExpression>()) {
        auto pe0 = expr0->to<IR::PathExpression>();
        auto pe1 = expr1->to<IR::PathExpression>();
        return pe0->path == pe1->path;
    } else if (expr0->is<IR::Member>()) {
        auto mem0 = expr0->to<IR::Member>();
        auto mem1 = expr1->to<IR::Member>();
        return checkSame(mem0->expr, mem1->expr) && mem0->member == mem1->member;
    }
    BUG("%1%: unexpected expression for meter destination", expr0);
}

void DirectMeterMap::setDestination(const IR::IDeclaration* meter,
                                    const IR::Expression* destination) {
    auto info = getInfo(meter);
    if (info == nullptr)
        info = createInfo(meter);
    if (info->destinationField == nullptr) {
        info->destinationField = destination;
    } else {
        bool same = checkSame(destination, info->destinationField);
        if (!same)
            ::error("On this target all meter operations must write to the same destination ",
                    "but %1% and %2% are different", destination, info->destinationField);
    }
}

void DirectMeterMap::setSize(const IR::IDeclaration* meter, unsigned size) {
    auto info = getInfo(meter);
    CHECK_NULL(info);
    info->tableSize = size;
}

static cstring stringRepr(mpz_class value, unsigned bytes = 0) {
    cstring sign = "";
    const char* r;
    cstring filler = "";
    if (value < 0) {
        value =- value;
        r = mpz_get_str(nullptr, 16, value.get_mpz_t());
        sign = "-";
    } else {
        r = mpz_get_str(nullptr, 16, value.get_mpz_t());
    }

    if (bytes > 0) {
        int digits = bytes * 2 - strlen(r);
        BUG_CHECK(digits >= 0, "Cannot represent %1% on %2% bytes", value, bytes);
        filler = std::string(digits, '0');
    }
    return sign + "0x" + filler + r;
}

static Util::JsonObject* mkPrimitive(cstring name, Util::JsonArray* appendTo) {
    auto result = new Util::JsonObject();
    result->emplace("op", name);
    appendTo->append(result);
    return result;
}

static Util::JsonArray* mkArrayField(Util::JsonObject* parent, cstring name) {
    auto result = new Util::JsonArray();
    parent->emplace(name, result);
    return result;
}

static Util::JsonArray* pushNewArray(Util::JsonArray* parent) {
    auto result = new Util::JsonArray();
    parent->append(result);
    return result;
}

static Util::JsonArray* mkParameters(Util::JsonObject* object) {
    return mkArrayField(object, "parameters");
}

// Convert each expression into Json
// Place corresponding result in map.
class ExpressionConverter : public Inspector {
    std::map<const IR::Expression*, Util::IJson*> map;
    JsonConverter* converter;

 public:
    explicit ExpressionConverter(JsonConverter* converter) :
            converter(converter), simpleExpressionsOnly(false) {}
    bool simpleExpressionsOnly;  // if set we fail to convert complex expressions

    Util::IJson* get(const IR::Expression* expression) const {
        auto result = ::get(map, expression);
        BUG_CHECK(result, "%1%: could not convert to Json", expression);
        return result;
    }

    void postorder(const IR::BoolLiteral* expression) override {
        auto result = new Util::JsonObject();
        result->emplace("type", "bool");
        result->emplace("value", expression->value);
        map.emplace(expression, result);
    }

    void postorder(const IR::MethodCallExpression* expression) override {
        auto instance = P4::MethodInstance::resolve(
            expression, converter->refMap, converter->typeMap);
        if (instance->is<P4::ExternMethod>()) {
            auto em = instance->to<P4::ExternMethod>();
            if (em->type->name == converter->corelib.packetIn.name &&
                em->method->name == converter->corelib.packetIn.lookahead.name) {
                BUG_CHECK(expression->typeArguments->size() == 1,
                          "Expected 1 type parameter for %1%", em->method);
                auto targ = expression->typeArguments->at(0);
                auto typearg = converter->typeMap->getType(targ, true);
                int width = typearg->width_bits();
                BUG_CHECK(width > 0, "%1%: unknown width", targ);
                auto j = new Util::JsonObject();
                j->emplace("value", "lookahead");
                j->emplace("size", width);
                map.emplace(expression, j);
                return;
            }
        } else if (instance->is<P4::BuiltInMethod>()) {
            auto bim = instance->to<P4::BuiltInMethod>();
            if (bim->name == IR::Type_Header::isValid) {
                auto result = new Util::JsonObject();
                result->emplace("type", "expression");
                auto e = new Util::JsonObject();
                result->emplace("value", e);
                e->emplace("op", "valid");
                e->emplace("left", Util::JsonValue::null);
                auto l = get(bim->appliedTo);
                e->emplace("right", l);
                map.emplace(expression, result);
                return;
            }
        }
        BUG("%1%: unhandled case", expression);
    }

    void postorder(const IR::Cast* expression) override {
        auto desttype = converter->typeMap->getType(expression->type, true);
        auto srctype = converter->typeMap->getType(expression->expr, true);
        // some casts just "work" without doing anything
        if (desttype->is<IR::Type_Bits>() && srctype->is<IR::Type_Bits>()) {
            auto db = desttype->to<IR::Type_Bits>();
            auto sb = srctype->to<IR::Type_Bits>();
            if (db->isSigned != sb->isSigned ||
                (db->isSigned && sb->isSigned && db->size > sb->size))
                ::warning("%1%: Casts not supported on this target; will do best-effort conversion",
                          expression);
        }
        auto j = get(expression->expr);
        map.emplace(expression, j);
    }

    void postorder(const IR::Slice* expression) override {
        // Special case for parser select: look for
        // packet.lookahead<T>()[h:l].  Convert to current(l, h - l).
        // Only correct within a select() expression, but we cannot check that
        // since the caller invokes the converter directly with the select argument.
        auto m = get(expression->e0);
        if (m->is<Util::JsonObject>()) {
            auto val = m->to<Util::JsonObject>()->get("value");
            if (val != nullptr && val->is<Util::JsonValue>() &&
                *val->to<Util::JsonValue>() == "lookahead") {
                int h = expression->getH();
                int l = expression->getL();
                auto j = new Util::JsonObject();
                j->emplace("type", "lookahead");
                auto bounds = mkArrayField(j, "value");
                bounds->append(l);
                bounds->append(h + 1);
                map.emplace(expression, j);
                return;
            }
        }
        BUG("%1%: unhandled case", expression);
    }

    void postorder(const IR::Constant* expression) override {
        auto result = new Util::JsonObject();
        result->emplace("type", "hexstr");

        cstring repr = stringRepr(expression->value,
                                  ROUNDUP(expression->type->width_bits(), 8));
        result->emplace("value", repr);
        map.emplace(expression, result);
    }

    void postorder(const IR::ArrayIndex* expression) override {
        auto result = new Util::JsonObject();
        result->emplace("type", "header");
        // This is supposed to be a header, which is supposed to be a member of the headers struct.
        auto mem = expression->left->to<IR::Member>();
        BUG_CHECK(mem != nullptr, "%1%: expected array indexed", expression->left);
        cstring elementAccess = mem->member.name;

        if (!expression->right->is<IR::Constant>()) {
            ::error("%1%: all array indexes must be constant on this architecture",
                    expression->right);
        } else {
            int index = expression->right->to<IR::Constant>()->asInt();
            elementAccess += "[" + Util::toString(index) + "]";
        }
        result->emplace("value", elementAccess);
        map.emplace(expression, result);
    }

    bool isParamReference(const IR::Expression* expression) {
        CHECK_NULL(expression);
        if (!expression->is<IR::PathExpression>())
            return false;

        auto pe = expression->to<IR::PathExpression>();
        auto decl = converter->refMap->getDeclaration(pe->path, true);
        auto param = decl->to<IR::Parameter>();
        if (param == nullptr)
            return false;
        if (param->name.name == converter->v1model.standardMetadata.name)
            // Treat 'standard_metadata' specially
            return false;
        return converter->structure.nonActionParameters.count(param) > 0;
    }

    void postorder(const IR::Member* expression) override {
        auto result = new Util::JsonObject();
        if (isParamReference(expression->expr)) {
            auto type = converter->typeMap->getType(expression, true);
            if (type->is<IR::Type_Stack>())
                result->emplace("type", "header_stack");
            else
                // This may be wrong, but the caller will handle it properly
                // (e.g., this can be a method, such as packet.lookahead)
                result->emplace("type", "header");
            result->emplace("value", expression->member.name);
        } else {
            bool done = false;
            if (expression->expr->is<IR::Member>()) {
                // array.next.field => type: "stack_field", value: [ array, field ]
                auto mem = expression->expr->to<IR::Member>();
                auto memtype = converter->typeMap->getType(mem->expr, true);
                if (memtype->is<IR::Type_Stack>() && mem->member == IR::Type_Stack::last) {
                    auto l = get(mem->expr);
                    CHECK_NULL(l);
                    result->emplace("type", "stack_field");
                    auto e = mkArrayField(result, "value");
                    if (l->is<Util::JsonObject>())
                        e->append(l->to<Util::JsonObject>()->get("value"));
                    else
                        e->append(l);
                    e->append(expression->member);
                    done = true;
                }
            }

            if (!done) {
                auto l = get(expression->expr);
                CHECK_NULL(l);
                result->emplace("type", "field");
                auto e = mkArrayField(result, "value");
                if (l->is<Util::JsonObject>())
                    e->append(l->to<Util::JsonObject>()->get("value"));
                else
                    e->append(l);
                e->append(expression->member.name);
            }
        }
        map.emplace(expression, result);
    }

    static Util::IJson* fixLocal(Util::IJson* json) {
        if (json->is<Util::JsonObject>()) {
            auto jo = json->to<Util::JsonObject>();
            auto to = jo->get("type");
            if (to != nullptr && to->to<Util::JsonValue>() != nullptr &&
                (*to->to<Util::JsonValue>()) == "runtime_data") {
                auto result = new Util::JsonObject();
                result->emplace("type", "local");
                result->emplace("value", jo->get("value"));
                return result;
            }
        }
        return json;
    }

    void postorder(const IR::Mux* expression) override {
        auto result = new Util::JsonObject();
        map.emplace(expression, result);
        if (simpleExpressionsOnly) {
            ::error("%1%: expression to complex for this target", expression);
            return;
        }

        result->emplace("type", "expression");
        auto e = new Util::JsonObject();
        result->emplace("value", e);
        e->emplace("op", "?");
        auto l = get(expression->e1);
        e->emplace("left", fixLocal(l));
        auto r = get(expression->e2);
        e->emplace("right", fixLocal(r));
        auto c = get(expression->e0);
        e->emplace("cond", fixLocal(c));
    }

    void postorder(const IR::Operation_Binary* expression) override {
        auto result = new Util::JsonObject();
        map.emplace(expression, result);
        if (simpleExpressionsOnly) {
            ::error("%1%: expression to complex for this target", expression);
            return;
        }

        result->emplace("type", "expression");
        auto e = new Util::JsonObject();
        result->emplace("value", e);
        cstring op = expression->getStringOp();
        if (op == "&&")
            op = "and";
        else if (op == "||")
            op = "or";
        e->emplace("op", op);
        auto l = get(expression->left);
        e->emplace("left", fixLocal(l));
        auto r = get(expression->right);
        e->emplace("right", fixLocal(r));
    }

    void postorder(const IR::ListExpression* expression) override {
        auto result = new Util::JsonArray();
        map.emplace(expression, result);
        if (simpleExpressionsOnly) {
            ::error("%1%: expression to complex for this target", expression);
            return;
        }

        for (auto e : *expression->components) {
            auto t = get(e);
            result->append(t);
        }
    }

    void postorder(const IR::Operation_Unary* expression) override {
        auto result = new Util::JsonObject();
        map.emplace(expression, result);
        if (simpleExpressionsOnly) {
            ::error("%1%: expression to complex for this target", expression);
            return;
        }

        result->emplace("type", "expression");
        auto e = new Util::JsonObject();
        result->emplace("value", e);
        cstring op = expression->getStringOp();
        if (op == "!")
            op = "not";
        e->emplace("op", op);
        e->emplace("left", Util::JsonValue::null);
        auto r = get(expression->expr);
        e->emplace("right", fixLocal(r));
    }

    void postorder(const IR::PathExpression* expression) override {
        // This is useful for action bodies mostly
        auto decl = converter->refMap->getDeclaration(expression->path, true);
        if (decl->is<IR::Parameter>()) {
            auto param = decl->to<IR::Parameter>();
            if (converter->structure.nonActionParameters.find(param) !=
                converter->structure.nonActionParameters.end()) {
                map.emplace(expression, new Util::JsonValue(param->name.name));
                return;
            }

            auto result = new Util::JsonObject();
            result->emplace("type", "runtime_data");
            unsigned paramIndex = ::get(converter->structure.index, param);
            result->emplace("value", paramIndex);
            map.emplace(expression, result);
        }
    }

    void postorder(const IR::Expression* expression) override {
        BUG("%1%: Unhandled case", expression);
    }

    Util::IJson* convert(const IR::Expression* e) {
        e->apply(*this);
        auto result = ::get(map, e);
        if (result != nullptr)
            return result;
        BUG("%1%: Error in conversion", e);
    }
};

JsonConverter::JsonConverter(const CompilerOptions& options) :
        options(options), v1model(P4V1::V1Model::instance),
        corelib(P4::P4CoreLibrary::instance),
        refMap(nullptr), typeMap(nullptr), conv(new ExpressionConverter(this)) {}

// return calculation name
cstring JsonConverter::createCalculation(cstring algo, const IR::Expression* fields,
                                         Util::JsonArray* calculations) {
    cstring calcName = refMap->newName("calc_");
    auto calc = new Util::JsonObject();
    calc->emplace("name", calcName);
    calc->emplace("id", nextId("calculations"));
    calc->emplace("algo", algo);
    auto jright = conv->convert(fields);
    calc->emplace("input", jright);
    calculations->append(calc);
    return calcName;
}

static Util::IJson* wrapExpression(Util::IJson* json) {
    // This is weird, but that's how it is
    if (json->is<Util::JsonObject>()) {
        auto to = json->to<Util::JsonObject>()->get("type");
        if (to != nullptr && to->to<Util::JsonValue>() != nullptr &&
            (*to->to<Util::JsonValue>()) == "expression") {
            auto rwrap = new Util::JsonObject();
            rwrap->emplace("type", "expression");
            rwrap->emplace("value", json);
            json = rwrap;
        }
    }
    return json;
}

void
JsonConverter::convertActionBody(const IR::Vector<IR::StatOrDecl>* body,
                                 Util::JsonArray* result, Util::JsonArray* fieldLists,
                                 Util::JsonArray* calculations, Util::JsonArray* learn_lists) {
    for (auto s : *body) {
        if (!s->is<IR::Statement>()) {
            ::error("%1%: Declarations in actions are not supported on this target", s);
            continue;
        } else if (s->is<IR::BlockStatement>()) {
            convertActionBody(s->to<IR::BlockStatement>()->components, result, fieldLists,
                              calculations, learn_lists);
            continue;
        } else if (s->is<IR::ReturnStatement>()) {
            break;
        } else if (s->is<IR::ExitStatement>()) {
            auto primitive = mkPrimitive("exit", result);
            (void)mkParameters(primitive);
            break;
        } else if (s->is<IR::AssignmentStatement>()) {
            const IR::Expression* l, *r;
            auto assign = s->to<IR::AssignmentStatement>();
            l = assign->left;
            r = assign->right;

            cstring operation;
            auto type = typeMap->getType(l, true);
            if (type->is<IR::Type_Header>())
                operation = "copy_header";
            else
                operation = "modify_field";
            auto primitive = mkPrimitive(operation, result);
            auto parameters = mkParameters(primitive);
            auto left = conv->convert(l);
            left = wrapExpression(left);
            parameters->append(left);
            auto right = conv->convert(r);
            right = wrapExpression(right);
            parameters->append(right);
            continue;
        } else if (s->is<IR::EmptyStatement>()) {
            continue;
        } else if (s->is<IR::MethodCallStatement>()) {
            auto mc = s->to<IR::MethodCallStatement>()->methodCall;
            auto mi = P4::MethodInstance::resolve(mc, refMap, typeMap);
            if (mi->is<P4::ActionCall>()) {
                BUG("%1%: action call should have been inlined", mc);
                continue;
            } else if (mi->is<P4::BuiltInMethod>()) {
                auto builtin = mi->to<P4::BuiltInMethod>();

                cstring prim;
                auto parameters = new Util::JsonArray();
                BUG_CHECK(mc->arguments->size() == 1, "Expected 1 argument for %1%", mc);
                auto arg = conv->convert(mc->arguments->at(0));
                auto obj = conv->convert(builtin->appliedTo);
                parameters->append(obj);

                if (builtin->name == IR::Type_Header::setValid) {
                    if (!mc->arguments->at(0)->is<IR::BoolLiteral>()) {
                        ::error("%1%: argument must be a constant for this target", mc);
                        continue;
                    }
                    auto bl = mc->arguments->at(0)->to<IR::BoolLiteral>();
                    if (bl->value)
                        prim = "add_header";
                    else
                        prim = "remove_header";
                } else if (builtin->name == IR::Type_Stack::push_front) {
                    prim = "push";
                    parameters->append(arg);
                } else if (builtin->name == IR::Type_Stack::pop_front) {
                    prim = "pop";
                    parameters->append(arg);
                } else {
                    BUG("%1%: Unexpected built-in method", s);
                }
                auto primitive = mkPrimitive(prim, result);
                primitive->emplace("parameters", parameters);
                continue;
            } else if (mi->is<P4::ExternMethod>()) {
                auto em = mi->to<P4::ExternMethod>();
                if (em->type->name == v1model.counter.name) {
                    if (em->method->name == v1model.counter.increment.name) {
                        BUG_CHECK(mc->arguments->size() == 1, "Expected 1 argument for %1%", mc);
                        auto primitive = mkPrimitive("count", result);
                        auto parameters = mkParameters(primitive);
                        auto ctr = new Util::JsonObject();
                        ctr->emplace("type", "counter_array");
                        ctr->emplace("value", em->object->getName());
                        parameters->append(ctr);
                        auto index = conv->convert(mc->arguments->at(0));
                        parameters->append(index);
                        continue;
                    }
                } else if (em->type->name == v1model.meter.name) {
                    if (em->method->name == v1model.meter.executeMeter.name) {
                        BUG_CHECK(mc->arguments->size() == 2, "Expected 2 arguments for %1%", mc);
                        auto primitive = mkPrimitive("execute_meter", result);
                        auto parameters = mkParameters(primitive);
                        auto mtr = new Util::JsonObject();
                        mtr->emplace("type", "meter_array");
                        mtr->emplace("value", em->object->getName());
                        parameters->append(mtr);
                        auto index = conv->convert(mc->arguments->at(0));
                        parameters->append(index);
                        auto result = conv->convert(mc->arguments->at(1));
                        parameters->append(result);
                        continue;
                    }
                } else if (em->type->name == v1model.registers.name) {
                    BUG_CHECK(mc->arguments->size() == 2, "Expected 2 arguments for %1%", mc);
                    auto reg = new Util::JsonObject();
                    reg->emplace("type", "register_array");
                    reg->emplace("value", em->object->getName());
                    if (em->method->name == v1model.registers.read.name) {
                        auto primitive = mkPrimitive("register_read", result);
                        auto parameters = mkParameters(primitive);
                        auto dest = conv->convert(mc->arguments->at(0));
                        parameters->append(dest);
                        parameters->append("reg");
                        auto index = conv->convert(mc->arguments->at(1));
                        parameters->append(index);
                        continue;
                    } else if (em->method->name == v1model.registers.write.name) {
                        auto primitive = mkPrimitive("register_write", result);
                        auto parameters = mkParameters(primitive);
                        parameters->append(reg);
                        auto index = conv->convert(mc->arguments->at(0));
                        parameters->append(index);
                        auto value = conv->convert(mc->arguments->at(1));
                        parameters->append(value);
                        continue;
                    }
                } else if (em->type->name == v1model.directMeter.name) {
                    if (em->method->name == v1model.directMeter.read.name) {
                        BUG_CHECK(mc->arguments->size() == 1, "Expected 1 argument for %1%", mc);
                        auto dest = mc->arguments->at(0);
                        meterMap.setDestination(em->object, dest);
                        // Do not generate any code for this operation
                        continue;
                    }
                }
            } else if (mi->is<P4::ExternFunction>()) {
                auto ef = mi->to<P4::ExternFunction>();
                if (ef->method->name == v1model.clone.name ||
                    ef->method->name == v1model.clone.clone3.name) {
                    int id = -1;
                    if (ef->method->name == v1model.clone.name) {
                        BUG_CHECK(mc->arguments->size() == 2, "Expected 2 arguments for %1%", mc);
                    } else {
                        BUG_CHECK(mc->arguments->size() == 3, "Expected 3 arguments for %1%", mc);
                        cstring name = refMap->newName("fl");
                        id = createFieldList(mc->arguments->at(2), "field_lists", name, fieldLists);
                    }
                    auto cloneType = mc->arguments->at(0);
                    auto ei = P4::EnumInstance::resolve(cloneType, typeMap);
                    if (ei == nullptr) {
                        ::error("%1%: must be a constant on this target", cloneType);
                    } else {
                        cstring prim = ei->name == "I2E" ? "clone_ingress_pkt_to_egress" :
                                "clone_egress_pkt_to_egress";
                        auto session = conv->convert(mc->arguments->at(1));
                        auto primitive = mkPrimitive(prim, result);
                        auto parameters = mkParameters(primitive);
                        parameters->append(session);

                        if (id >= 0) {
                            auto cst = new IR::Constant(id);
                            auto jcst = conv->convert(cst);
                            parameters->append(jcst);
                        }
                    }
                    continue;
                } else if (ef->method->name == v1model.hash.name) {
                    BUG_CHECK(mc->arguments->size() == 5, "Expected 5 arguments for %1%", mc);
                    auto primitive = mkPrimitive("modify_field_with_hash_based_offset", result);
                    auto parameters = mkParameters(primitive);
                    auto dest = conv->convert(mc->arguments->at(0));
                    parameters->append(dest);
                    auto base = conv->convert(mc->arguments->at(2));
                    parameters->append(base);
                    auto calc = new Util::JsonObject();
                    auto ei = P4::EnumInstance::resolve(mc->arguments->at(1), typeMap);
                    CHECK_NULL(ei);
                    cstring algo = convertHashAlgorithm(ei->name);
                    cstring calcName = createCalculation(algo, mc->arguments->at(3), calculations);
                    calc->emplace("type", "calculation");
                    calc->emplace("value", calcName);
                    parameters->append(calc);
                    auto max = conv->convert(mc->arguments->at(4));
                    parameters->append(max);
                    continue;
                } else if (ef->method->name == v1model.digest_receiver.name) {
                    BUG_CHECK(mc->arguments->size() == 2, "Expected 2 arguments for %1%", mc);
                    auto primitive = mkPrimitive("generate_digest", result);
                    auto parameters = mkParameters(primitive);
                    auto dest = conv->convert(mc->arguments->at(0));
                    parameters->append(dest);
                    cstring listName = "digest";
                    // If we are supplied a type argument that is a named type use
                    // that for the list name.
                    if (mc->typeArguments->size() == 1) {
                        auto typeArg = mc->typeArguments->at(0);
                        if (typeArg->is<IR::Type_Name>()) {
                            auto origType = refMap->getDeclaration(
                                typeArg->to<IR::Type_Name>()->path);
                            CHECK_NULL(origType);
                            BUG_CHECK(origType->is<IR::Type_Struct>(),
                                      "%1%: expected a struct type", origType);
                            auto st = origType->to<IR::Type_Struct>();
                            listName = nameFromAnnotation(st->annotations, st->name);
                        }
                    }
                    int id = createFieldList(mc->arguments->at(1), "learn_lists",
                                             listName, learn_lists);
                    auto cst = new IR::Constant(id);
                    auto jcst = conv->convert(cst);
                    parameters->append(jcst);
                    continue;
                } else if (ef->method->name == v1model.resubmit.name ||
                           ef->method->name == v1model.recirculate.name) {
                    BUG_CHECK(mc->arguments->size() == 1, "Expected 1 argument for %1%", mc);
                    cstring prim = (ef->method->name == v1model.resubmit.name) ?
                            "resubmit" : "recirculate";
                    auto primitive = mkPrimitive(prim, result);
                    auto parameters = mkParameters(primitive);
                    cstring listName = prim;
                    // If we are supplied a type argument that is a named type use
                    // that for the list name.
                    if (mc->typeArguments->size() == 1) {
                        auto typeArg = mc->typeArguments->at(0);
                        if (typeArg->is<IR::Type_Name>()) {
                            auto origType = refMap->getDeclaration(
                                typeArg->to<IR::Type_Name>()->path);
                            CHECK_NULL(origType);
                            BUG_CHECK(origType->is<IR::Type_Struct>(),
                                      "%1%: expected a struct type", origType);
                            auto st = origType->to<IR::Type_Struct>();
                            listName = nameFromAnnotation(st->annotations, st->name);
                        }
                    }
                    int id = createFieldList(mc->arguments->at(0), "field_lists",
                                             listName, fieldLists);
                    auto cst = new IR::Constant(id);
                    auto jcst = conv->convert(cst);
                    parameters->append(jcst);
                    continue;
                } else if (ef->method->name == v1model.drop.name) {
                    BUG_CHECK(mc->arguments->size() == 0, "Expected 0 arguments for %1%", mc);
                    auto primitive = mkPrimitive("drop", result);
                    (void)mkParameters(primitive);
                    continue;
                }
            }
        }
        ::error("%1%: not yet supported on this target", s);
    }
}

void JsonConverter::addToFieldList(const IR::Expression* expr, Util::JsonArray* fl) {
    if (expr->is<IR::ListExpression>()) {
        auto le = expr->to<IR::ListExpression>();
        for (auto e : *le->components) {
            addToFieldList(e, fl);
        }
        return;
    }

    auto type = typeMap->getType(expr, true);
    if (type->is<IR::Type_StructLike>()) {
        // recursively add all fields
        auto st = type->to<IR::Type_StructLike>();
        for (auto f : *st->getEnumerator()) {
            auto member = new IR::Member(Util::SourceInfo(), expr, f->name);
            typeMap->setType(member, typeMap->getType(f, true));
            addToFieldList(member, fl);
        }
        return;
    }

    auto j = conv->convert(expr);
    fl->append(j);
}

// returns id of created field list
int JsonConverter::createFieldList(const IR::Expression* expr, cstring group, cstring listName,
                                   Util::JsonArray* fieldLists) {
    auto fl = new Util::JsonObject();
    fieldLists->append(fl);
    int id = nextId(group);
    fl->emplace("id", id);
    fl->emplace("name", listName);
    auto elements = mkArrayField(fl, "elements");
    addToFieldList(expr, elements);
    return id;
}

Util::JsonArray* JsonConverter::createActions(Util::JsonArray* fieldLists,
                                              Util::JsonArray* calculations,
                                              Util::JsonArray* learn_lists) {
    auto result = new Util::JsonArray();
    for (auto it : structure.actions) {
        auto action = it.first;
        cstring name = nameFromAnnotation(action->annotations, action->name);
        auto jact = new Util::JsonObject();
        jact->emplace("name", name);
        unsigned id = nextId("actions");
        structure.ids.emplace(action, id);
        jact->emplace("id", id);
        auto params = mkArrayField(jact, "runtime_data");
        for (auto p : *action->parameters->getEnumerator()) {
            // The P4 v1.0 compiler removes unused action parameters!
            // We have to do the same, although this seems wrong.
            if (!refMap->isUsed(p)) {
                ::warning("Removing unused action parameter %1% for compatibility reasons", p);
                continue;
            }

            auto param = new Util::JsonObject();
            param->emplace("name", p->name);
            auto type = typeMap->getType(p, true);
            if (!type->is<IR::Type_Bits>())
                ::error("%1%: Action parameters can only be bit<> or int<> on this target", p);
            param->emplace("bitwidth", type->width_bits());
            params->append(param);
        }
        auto body = mkArrayField(jact, "primitives");
        convertActionBody(action->body, body, fieldLists, calculations, learn_lists);
        result->append(jact);
    }
    return result;
}

Util::IJson* JsonConverter::nodeName(const CFG::Node* node) const
{ return new Util::JsonValue(node->name); }

Util::IJson* JsonConverter::convertIf(const CFG::IfNode* node, cstring) {
    auto result = new Util::JsonObject();
    result->emplace("name", node->name);
    result->emplace("id", nextId("conditionals"));
    auto j = conv->convert(node->statement->condition);
    CHECK_NULL(j);
    result->emplace("expression", j);
    for (auto e : node->successors.edges) {
        Util::IJson* dest = nodeName(e->endpoint);
        cstring label = Util::toString(e->getBool());
        label += "_next";
        result->emplace(label, dest);
    }
    return result;
}

void JsonConverter::handleTableImplementation(const IR::TableProperty* implementation,
                                              const IR::Key* key,
                                              Util::JsonObject* table) {
    cstring actionProfiles;
    if (implementation == nullptr) {
        actionProfiles = "simple";
        table->emplace("type", actionProfiles);
        return;
    }

    cstring name = refMap->newName("action_profile");
    name = nameFromAnnotation(implementation->annotations, name);
    table->emplace("act_prof_name", name);
    if (!implementation->value->is<IR::ExpressionValue>()) {
        ::error("%1%: expected expression for property", implementation);
        return;
    }
    auto propv = implementation->value->to<IR::ExpressionValue>();
    if (!propv->expression->is<IR::ConstructorCallExpression>()) {
        ::error("%1%: expected constructor call for property", implementation);
        return;
    }
    auto cc = P4::ConstructorCall::resolve(
        propv->expression->to<IR::ConstructorCallExpression>(), typeMap);
    if (!cc->is<P4::ExternConstructorCall>()) {
        ::error("%1%: expected extern object for property", implementation);
        return;
    }
    auto ecc = cc->to<P4::ExternConstructorCall>();
    if (ecc->type->name == v1model.action_selector.name) {
        BUG_CHECK(ecc->cce->arguments->size() == 3, "%1%: expected 3 arguments", cc->cce);

        auto selector = new Util::JsonObject();
        table->emplace("type", "indirect_ws");
        table->emplace("selector", selector);
        auto hash = ecc->cce->arguments->at(0);
        auto ei = P4::EnumInstance::resolve(hash, typeMap);
        if (ei == nullptr) {
            ::error("%1%: must be a constant on this target", hash);
        } else {
            cstring algo = convertHashAlgorithm(ei->name);
            selector->emplace("algo", algo);
        }
        auto input = mkArrayField(selector, "input");
        for (auto ke : *key->keyElements) {
            auto mt = refMap->getDeclaration(ke->matchType->path, true)->to<IR::Declaration_ID>();
            BUG_CHECK(mt != nullptr, "%1%: could not find declaration", ke->matchType);
            if (mt->name.name != v1model.selectorMatchType.name)
                continue;

            auto expr = ke->expression;
            auto jk = conv->convert(expr);
            input->append(jk);
        }
    } else if (ecc->type->name == v1model.action_profile.name) {
        table->emplace("type", "indirect");
    } else {
        ::error("%1%: unexpected value for property", propv);
    }
}

cstring JsonConverter::convertHashAlgorithm(cstring algorithm) const {
    cstring result;
    if (algorithm == v1model.algorithm.crc32.name)
        result = "crc32";
    else if (algorithm == v1model.algorithm.crc16.name)
        result = "crc16";
    else if (algorithm == v1model.algorithm.random.name)
        result = "random";
    else if (algorithm == v1model.algorithm.identity.name)
        result = "identity";
    else
        ::error("%1%: unexpected algorithm", algorithm);
    return result;
}

Util::IJson*
JsonConverter::convertTable(const CFG::TableNode* node, Util::JsonArray* counters) {
    auto table = node->table;
    LOG1("Processing " << table);
    auto result = new Util::JsonObject();
    cstring name = nameFromAnnotation(table->annotations, table->name);
    result->emplace("name", name);
    result->emplace("id", nextId("tables"));
    cstring table_match_type = "exact";
    auto key = table->getKey();
    auto tkey = mkArrayField(result, "key");
    conv->simpleExpressionsOnly = true;

    if (key != nullptr) {
        for (auto ke : *key->keyElements) {
            auto mt = refMap->getDeclaration(ke->matchType->path, true)->to<IR::Declaration_ID>();
            BUG_CHECK(mt != nullptr, "%1%: could not find declaration", ke->matchType);
            auto expr = ke->expression;
            mpz_class mask;
            if (expr->is<IR::Slice>()) {
                auto slice = expr->to<IR::Slice>();
                expr = slice->e0;
                int h = slice->getH();
                int l = slice->getL();
                mask = Util::maskFromSlice(h, l);
            }

            cstring match_type = mt->name.name;
            if (mt->name.name == corelib.exactMatch.name) {
                if (expr->is<IR::MethodCallExpression>()) {
                    auto mi = P4::MethodInstance::resolve(expr->to<IR::MethodCallExpression>(),
                                                          refMap, typeMap);
                    if (mi->is<P4::BuiltInMethod>()) {
                        auto bim = mi->to<P4::BuiltInMethod>();
                        if (bim->name == IR::Type_Header::isValid) {
                            expr = bim->appliedTo;
                            match_type = "valid";
                        }
                    }
                }
            } else if (mt->name.name == corelib.ternaryMatch.name) {
                if (table_match_type == "exact")
                    table_match_type = "ternary";
            } else if (mt->name.name == corelib.lpmMatch.name) {
                if (table_match_type != "lpm")
                    table_match_type = "lpm";
            } else if (mt->name.name == v1model.selectorMatchType.name) {
                continue;
            } else {
                ::error("%1%: match type not supported on this target", mt);
            }

            auto keyelement = new Util::JsonObject();
            keyelement->emplace("match_type", match_type);
            auto jk = conv->convert(expr);
            keyelement->emplace("target", jk->to<Util::JsonObject>()->get("value"));
            if (mask != 0)
                keyelement->emplace("mask", stringRepr(mask));
            else
                keyelement->emplace("mask", Util::JsonValue::null);
            tkey->append(keyelement);
        }
    }
    result->emplace("match_type", table_match_type);
    conv->simpleExpressionsOnly = false;

    auto impl = table->properties->getProperty(v1model.tableAttributes.tableImplementation.name);
    handleTableImplementation(impl, key, result);

    unsigned size = 0;
    auto sz = table->properties->getProperty(v1model.tableAttributes.size.name);
    if (sz != nullptr) {
        if (sz->value->is<IR::ExpressionValue>()) {
            auto expr = sz->value->to<IR::ExpressionValue>()->expression;
            if (!expr->is<IR::Constant>()) {
                ::error("%1% must be a constant", sz);
                size = 0;
            } else {
                size = expr->to<IR::Constant>()->asInt();
            }
        } else {
            ::error("%1%: expected a number", sz);
        }
    }
    if (size == 0)
        size = v1model.tableAttributes.defaultTableSize;

    result->emplace("max_size", size);
    auto ctrs = table->properties->getProperty(v1model.tableAttributes.directCounter.name);
    if (ctrs != nullptr) {
        result->emplace("with_counters", true);
        auto jctr = new Util::JsonObject();
        cstring ctrname = nameFromAnnotation(ctrs->annotations, "counter");
        jctr->emplace("name", ctrname);
        jctr->emplace("id", nextId("counter_arrays"));
        jctr->emplace("is_direct", true);
        jctr->emplace("binding", name);
        counters->append(jctr);
    } else {
        result->emplace("with_counters", false);
    }

    bool sup_to = false;
    auto timeout = table->properties->getProperty(v1model.tableAttributes.supportTimeout.name);
    if (timeout != nullptr) {
        if (timeout->value->is<IR::ExpressionValue>()) {
            auto expr = timeout->value->to<IR::ExpressionValue>()->expression;
            if (!expr->is<IR::BoolLiteral>()) {
                ::error("%1% must be true/false", timeout);
            } else {
                sup_to = expr->to<IR::BoolLiteral>()->value;
            }
        } else {
            ::error("%1%: expected a Boolean", timeout);
        }
    }
    result->emplace("support_timeout", sup_to);

    auto dm = table->properties->getProperty(v1model.tableAttributes.directMeter.name);
    if (dm != nullptr) {
        if (dm->value->is<IR::ExpressionValue>()) {
            auto expr = dm->value->to<IR::ExpressionValue>()->expression;
            if (!expr->is<IR::PathExpression>()) {
                ::error("%1%: expected a reference to a meter declaration", expr);
            } else {
                auto pe = expr->to<IR::PathExpression>();
                auto decl = refMap->getDeclaration(pe->path, true);
                meterMap.setTable(decl, table);
                meterMap.setSize(decl, size);
                BUG_CHECK(decl->is<IR::Declaration_Instance>(),
                          "%1%: expected an instance", decl->getNode());
                cstring dmname = nameFromAnnotation(
                    decl->to<IR::Declaration_Instance>()->annotations, decl->getName());
                result->emplace("direct_meters", dmname);
            }
        } else {
            ::error("%1%: expected a Boolean", timeout);
        }
    } else {
        result->emplace("direct_meters", Util::JsonValue::null);
    }

    auto action_ids = mkArrayField(result, "action_ids");
    auto actions = mkArrayField(result, "actions");
    auto al = table->getActionList();

    std::map<cstring, cstring> useActionName;
    for (auto a : *al->actionList) {
        if (a->arguments != nullptr && a->arguments->size() > 0)
            ::error("%1%: Actions in action list with arguments not supported", a);
        auto decl = refMap->getDeclaration(a->name->path);
        CHECK_NULL(decl);
        BUG_CHECK(decl->is<IR::ActionContainer>(), "%1%: should be an action name", a);
        auto action = decl->to<IR::ActionContainer>();
        unsigned id = get(structure.ids, action);
        action_ids->append(id);
        auto name = nameFromAnnotation(action->annotations, action->name);
        actions->append(name);
        useActionName.emplace(action->name, name);
    }

    auto next_tables = new Util::JsonObject();

    CFG::Node* nextDestination = nullptr;  // if no action is executed
    CFG::Node* defaultLabelDestination = nullptr;  // if the "default" label is executed
    // Note: the "default" label is not the default_action.
    bool hitMiss = false;
    for (auto s : node->successors.edges) {
        if (s->isUnconditional())
            nextDestination = s->endpoint;
        else if (s->isBool())
            hitMiss = true;
        else if (s->label == IR::SwitchStatement::default_label)
            defaultLabelDestination = s->endpoint;
    }

    Util::IJson* nextLabel = nullptr;
    if (!hitMiss) {
        BUG_CHECK(nextDestination, "Could not find default destination for %1%", node->invocation);
        nextLabel = nodeName(nextDestination);
        result->emplace("base_default_next", nextLabel);
        // So if a "default:" switch case exists we set the nextLabel
        // to be the destination of the default: label.
        if (defaultLabelDestination != nullptr)
            nextLabel = nodeName(defaultLabelDestination);
    } else {
        result->emplace("base_default_next", Util::JsonValue::null);
    }

    std::set<cstring> labelsDone;
    for (auto s : node->successors.edges) {
        cstring label;
        if (s->isBool()) {
            label = s->getBool() ? "__HIT__" : "__MISS__";
        } else if (s->isUnconditional()) {
            continue;
        } else {
            label = s->label.name;
            if (label == IR::SwitchStatement::default_label)
                continue;
            label = ::get(useActionName, label);
        }
        next_tables->emplace(label, nodeName(s->endpoint));
        labelsDone.emplace(label);
    }

    // Generate labels which don't show up and send them to
    // the nextLabel.
    if (!hitMiss) {
        for (auto a : *al->actionList) {
            cstring name = a->name->path->name;
            cstring label = ::get(useActionName, name);
            if (labelsDone.find(label) == labelsDone.end())
                next_tables->emplace(label, nextLabel);
        }
    }

    result->emplace("next_tables", next_tables);
    result->emplace("default_action", Util::JsonValue::null);
    auto defact = table->properties->getProperty(IR::TableProperties::defaultActionPropertyName);
    if (defact != nullptr)
        ::warning("%1%: setting default action currently not supported", defact);
    return result;
}

Util::IJson* JsonConverter::convertControl(const IR::ControlBlock* block, cstring name,
                                           Util::JsonArray *counters, Util::JsonArray* meters,
                                           Util::JsonArray* registers) {
    const IR::ControlContainer* cont = block->container;
    LOG1("Processing " << cont);
    auto result = new Util::JsonObject();
    result->emplace("name", name);
    result->emplace("id", nextId("control"));

    auto cfg = new CFG();
    cfg->build(cont, refMap, typeMap);

    if (cfg->entryPoint->successors.size() == 0) {
        result->emplace("init_table", Util::JsonValue::null);
    } else {
        BUG_CHECK(cfg->entryPoint->successors.size() == 1, "Expected 1 start node for %1%", cont);
        auto start = (*(cfg->entryPoint->successors.edges.begin()))->endpoint;
        result->emplace("init_table", start->name);
    }
    auto tables = mkArrayField(result, "tables");
    auto conditionals = mkArrayField(result, "conditionals");

    for (auto node : cfg->allNodes) {
        if (node->is<CFG::TableNode>()) {
            auto j = convertTable(node->to<CFG::TableNode>(), counters);
            tables->append(j);
        } else if (node->is<CFG::IfNode>()) {
            auto j = convertIf(node->to<CFG::IfNode>(), cont->name);
            conditionals->append(j);
        }
    }

    {
        // synthesize an special table at the exit for dropping the packet
        auto exitTable = new Util::JsonObject();
        exitTable->emplace("name", nodeName(cfg->exitPoint));
        exitTable->emplace("id", nextId("tables"));
        auto key = mkArrayField(exitTable, "key");
        auto ke = new Util::JsonObject();
        key->append(ke);
        ke->emplace("match_type", "exact");
        auto drop = mkArrayField(ke, "target");
        drop->append("standard_metadata");
        drop->append("drop");
        exitTable->emplace("match_type", "exact");
        exitTable->emplace("type", "simple");
        exitTable->emplace("max_size", 1);
        exitTable->emplace("with_counters", false);
        exitTable->emplace("support_timeout", false);
        exitTable->emplace("direct_meters", Util::JsonValue::null);
        auto actions = mkArrayField(exitTable, "actions");
        actions->append(dropAction);
        auto action_ids = mkArrayField(exitTable, "action_ids");
        action_ids->append(dropActionId);
        auto next_tables = new Util::JsonObject();
        next_tables->emplace(dropAction, Util::JsonValue::null);
        exitTable->emplace("next_tables", next_tables);
        tables->append(exitTable);
    }

    for (auto c : *cont->statefulEnumerator()) {
        if (c->is<IR::Declaration_Constant>() ||
            c->is<IR::Declaration_Variable>() ||
            c->is<IR::ActionContainer>() ||
            c->is<IR::TableContainer>())
            continue;
        if (c->is<IR::Declaration_Instance>()) {
            auto bl = block->getValue(c);
            CHECK_NULL(bl);
            if (bl->is<IR::ExternBlock>()) {
                auto eb = bl->to<IR::ExternBlock>();
                if (eb->type->name == v1model.counter.name) {
                    auto jctr = new Util::JsonObject();
                    jctr->emplace("name", c->name);
                    jctr->emplace("id", nextId("counter_arrays"));
                    auto sz = eb->getParameterValue(v1model.counter.sizeParam.name);
                    CHECK_NULL(sz);
                    BUG_CHECK(sz->is<IR::Constant>(), "%1%: expected a constant", sz);
                    jctr->emplace("size", sz->to<IR::Constant>()->value);
                    jctr->emplace("is_direct", false);
                    counters->append(jctr);
                    continue;
                } else if (eb->type->name == v1model.meter.name) {
                    auto jmtr = new Util::JsonObject();
                    jmtr->emplace("name", c->name);
                    jmtr->emplace("id", nextId("meter_arrays"));
                    jmtr->emplace("is_direct", false);
                    auto sz = eb->getParameterValue(v1model.meter.sizeParam.name);
                    CHECK_NULL(sz);
                    BUG_CHECK(sz->is<IR::Constant>(), "%1%: expected a constant", sz);
                    jmtr->emplace("size", sz->to<IR::Constant>()->value);
                    jmtr->emplace("rate_count", 2);
                    auto mkind = eb->getParameterValue(v1model.meter.typeParam.name);
                    CHECK_NULL(mkind);
                    BUG_CHECK(mkind->is<IR::Declaration_ID>(), "%1%: expected a member", mkind);
                    cstring name = mkind->to<IR::Declaration_ID>()->name;
                    cstring type;
                    if (name == v1model.meter.counterType.packets.name)
                        type = "packets";
                    else if (name == v1model.meter.counterType.bytes.name)
                        type = "bytes";
                    else
                        type = "both";
                    jmtr->emplace("type", type);
                    meters->append(jmtr);
                    continue;
                } else if (eb->type->name == v1model.registers.name) {
                    auto jreg = new Util::JsonObject();
                    jreg->emplace("name", c->name);
                    jreg->emplace("id", nextId("register_arrays"));
                    auto sz = eb->getParameterValue(v1model.registers.sizeParam.name);
                    CHECK_NULL(sz);
                    BUG_CHECK(sz->is<IR::Constant>(), "%1%: expected a constant", sz);
                    jreg->emplace("size", sz->to<IR::Constant>()->value);
                    BUG_CHECK(eb->instanceType->is<IR::Type_SpecializedCanonical>(),
                              "%1%: Expected a generic specialized type", eb->instanceType);
                    auto st = eb->instanceType->to<IR::Type_SpecializedCanonical>();
                    BUG_CHECK(st->arguments->size() == 1, "%1%: expected 1 type argument");
                    unsigned width = st->arguments->at(0)->width_bits();
                    if (width == 0)
                        ::error("%1%: unknown width", st->arguments->at(0));
                    jreg->emplace("bitwidth", width);
                    registers->append(jreg);
                    continue;
                } else if (eb->type->name == v1model.directMeter.name) {
                    auto info = meterMap.getInfo(c);
                    CHECK_NULL(info);
                    CHECK_NULL(info->table);
                    CHECK_NULL(info->destinationField);

                    auto jmtr = new Util::JsonObject();
                    jmtr->emplace("name", c->name);
                    jmtr->emplace("id", nextId("meter_arrays"));
                    jmtr->emplace("is_direct", true);
                    jmtr->emplace("rate_count", 2);
                    auto mkind = eb->getParameterValue(v1model.directMeter.typeParam.name);
                    CHECK_NULL(mkind);
                    BUG_CHECK(mkind->is<IR::Declaration_ID>(), "%1%: expected a member", mkind);
                    cstring name = mkind->to<IR::Declaration_ID>()->name;
                    cstring type;
                    if (name == v1model.meter.counterType.packets.name)
                        type = "packets";
                    else if (name == v1model.meter.counterType.bytes.name)
                        type = "bytes";
                    else
                        type = "both";
                    jmtr->emplace("type", type);
                    jmtr->emplace("size", info->tableSize);
                    cstring tblname = nameFromAnnotation(info->table->annotations,
                                                         info->table->name);
                    jmtr->emplace("binding", tblname);
                    auto result = conv->convert(info->destinationField);
                    jmtr->emplace("result_target", result->to<Util::JsonObject>()->get("value"));
                    meters->append(jmtr);
                    continue;
                }
            }
        }
        BUG("%1%: not yet handled", c);
    }

    return result;
}

unsigned JsonConverter::nextId(cstring group) {
    static std::map<cstring, unsigned> counters;
    return counters[group]++;
}

void JsonConverter::addHeaderStacks(const IR::Type_Struct* headersStruct,
                                    Util::JsonArray* headers, Util::JsonArray* headerTypes,
                                    Util::JsonArray* stacks, std::set<cstring> &headerTypesDone) {
    for (auto f : *headersStruct->getEnumerator()) {
        auto ft = typeMap->getType(f->type, true);
        auto stack = ft->to<IR::Type_Stack>();
        if (stack == nullptr)
            continue;

        LOG1("Creating " << stack);
        auto json = new Util::JsonObject();
        json->emplace("name", nameFromAnnotation(f->annotations, f->name.name));
        json->emplace("id", nextId("stack"));
        json->emplace("size", stack->getSize());
        auto type = typeMap->getType(stack->baseType, true);
        BUG_CHECK(type->is<IR::Type_Header>(), "%1% not a header type", stack->baseType);
        auto ht = type->to<IR::Type_Header>();
        if (!headerTypesDone.count(ht->name)) {
            auto json = typeToJson(ht);
            headerTypes->append(json);
        }

        cstring header_type = stack->baseType->to<IR::Type_Header>()->name;
        json->emplace("header_type", header_type);
        auto stackMembers = mkArrayField(json, "header_ids");
        for (int i=0; i < stack->getSize(); i++) {
            unsigned id = nextId("headers");
            stackMembers->append(id);
            auto header = new Util::JsonObject();
            cstring name = nameFromAnnotation(f->annotations, f->name.name) +
                    "[" + Util::toString(i) + "]";
            header->emplace("name", name);
            header->emplace("id", id);
            header->emplace("header_type", header_type);
            header->emplace("metadata", false);
            headers->append(header);
        }
        stacks->append(json);
    }
}

void JsonConverter::convert(P4::BlockMap* bm) {
    blockMap = bm;
    refMap = blockMap->refMap;
    typeMap = blockMap->typeMap;

    auto package = blockMap->getMain();
    if (package->type->name != v1model.sw.name) {
        ::error("This back-end requires the program to be compiled for the %1% model",
                v1model.sw.name);
        return;
    }

    structure.analyze(blockMap);
    if (::errorCount() > 0)
        return;

    auto parserBlock = blockMap->getBlockBoundToParameter(package, v1model.sw.parser.name);
    CHECK_NULL(parserBlock);
    auto parser = parserBlock->to<IR::ParserBlock>()->container;
    auto hdr = parser->type->applyParams->getParameter(v1model.parser.headersParam.name);
    auto headersType = blockMap->typeMap->getType(hdr->getNode(), true);
    auto ht = headersType->to<IR::Type_Struct>();
    if (ht == nullptr) {
        ::error("Expected headers %1% to be a struct", headersType);
        return;
    }
    toplevel.emplace("program", options.file);

    auto headerTypes = mkArrayField(&toplevel, "header_types");
    auto headers = mkArrayField(&toplevel, "headers");
    auto headerStacks = mkArrayField(&toplevel, "header_stacks");
    auto fieldLists = mkArrayField(&toplevel, "field_lists");
    (void)nextId("field_lists");  // field list IDs must start at 1; 0 is reserved
    (void)nextId("learn_lists");  // idem

    std::set<cstring> headerTypesCreated;
    addTypesAndInstances(ht, false, headerTypes, headers, headerTypesCreated);
    addHeaderStacks(ht, headers, headerTypes, headerStacks, headerTypesCreated);

    auto md = parser->type->applyParams->getParameter(v1model.parser.metadataParam.name);
    auto mdType = blockMap->typeMap->getType(md->getNode(), true);
    auto mt = mdType->to<IR::Type_Struct>();
    if (mt == nullptr) {
        ::error("Expected metadata %1% to be a struct", mdType);
        return;
    }
    addTypesAndInstances(mt, true, headerTypes, headers, headerTypesCreated);

    auto prsrs = mkArrayField(&toplevel, "parsers");
    auto parserJson = toJson(parser);
    prsrs->append(parserJson);

    auto deparserBlock = blockMap->getBlockBoundToParameter(package, v1model.sw.deparser.name);
    CHECK_NULL(deparserBlock);
    auto deparser = deparserBlock->to<IR::ControlBlock>()->container;

    auto dprs = mkArrayField(&toplevel, "deparsers");
    auto deparserJson = convertDeparser(deparser);
    dprs->append(deparserJson);

    auto meters = mkArrayField(&toplevel, "meter_arrays");
    auto counters = mkArrayField(&toplevel, "counter_arrays");
    auto registers = mkArrayField(&toplevel, "register_arrays");
    auto calculations = mkArrayField(&toplevel, "calculations");
    auto learn_lists = mkArrayField(&toplevel, "learn_lists");

    auto acts = createActions(fieldLists, calculations, learn_lists);
    toplevel.emplace("actions", acts);
    {
        // synthesize a "drop" action
        auto drop = new Util::JsonObject();
        drop->emplace("name", dropAction);
        dropActionId = nextId("actions");
        drop->emplace("id", dropActionId);
        drop->emplace("runtime_data", new Util::JsonArray());
        auto body = mkArrayField(drop, "primitives");
        auto call = new Util::JsonObject();
        call->emplace("op", "drop");
        call->emplace("parameters", new Util::JsonArray());
        body->append(call);
        acts->append(drop);
    }

    auto pipelines = mkArrayField(&toplevel, "pipelines");
    auto ingressBlock = blockMap->getBlockBoundToParameter(package, v1model.sw.ingress.name);
    auto ingressControl = ingressBlock->to<IR::ControlBlock>();
    auto ingress = convertControl(ingressControl, v1model.ingress.name,
                                  counters, meters, registers);
    pipelines->append(ingress);
    auto egressBlock = blockMap->getBlockBoundToParameter(package, v1model.sw.egress.name);
    auto egress = convertControl(egressBlock->to<IR::ControlBlock>(),
                                 v1model.egress.name, counters, meters, registers);
    pipelines->append(egress);

    // standard metadata type and instance
    auto stdmeta = ingressControl->container->type->applyParams->getParameter(
        v1model.ingress.standardMetadataParam.name);
    auto stdMetaType = blockMap->typeMap->getType(stdmeta->getNode(), true);
    auto json = typeToJson(stdMetaType->to<IR::Type_StructLike>());
    headerTypes->append(json);

    {
        auto json = new Util::JsonObject();
        json->emplace("name", v1model.ingress.standardMetadataParam.name);
        json->emplace("id", nextId("headers"));
        json->emplace("header_type", stdMetaType->to<IR::Type_StructLike>()->name.name);
        json->emplace("metadata", true);
        headers->append(json);
    }

    auto checksums = mkArrayField(&toplevel, "checksums");
    auto updateBlock = blockMap->getBlockBoundToParameter(package, v1model.sw.update.name);
    CHECK_NULL(updateBlock);
    generateUpdate(updateBlock->to<IR::ControlBlock>()->container, checksums, calculations);

    // The whole P4 v1.0 spec about metadata is very confusing.
    // So this is rather heuristic...  Maybe these should be in v1model.p4.
    auto fa = mkArrayField(&toplevel, "force_arith");
    createForceArith(stdMetaType, v1model.standardMetadata.name, fa);
    std::vector<cstring> toForce = { "intrinsic_metadata", "queueing_metadata" };
    for (auto metaname : toForce) {
        auto im = mt->getField(metaname);
        if (im != nullptr) {
            auto type = typeMap->getType(im->type, true);
            createForceArith(type, metaname, fa);
        }
    }
}

void JsonConverter::createForceArith(const IR::Type* meta, cstring name,
                                     Util::JsonArray* force) const {
    BUG_CHECK(meta->is<IR::Type_Struct>(), "Expected a struct type");
    auto st = meta->to<IR::Type_StructLike>();
    for (auto f : *st->getEnumerator()) {
        auto field = pushNewArray(force);
        field->append(name);
        field->append(f->name.name);
    }
}

void JsonConverter::generateUpdate(const IR::ControlContainer* cont,
                                   Util::JsonArray* checksums, Util::JsonArray* calculations) {
    // Currently this is very hacky to target the very limited support for checksums in BMv2
    // This will work much better with an extern.
    for (auto stat : *cont->body->components) {
        if (stat->is<IR::IfStatement>()) {
            // The way checksums work in Json, they always ignore the condition!
            stat = stat->to<IR::IfStatement>()->ifTrue;
        }
        if (stat->is<IR::AssignmentStatement>()) {
            auto assign = stat->to<IR::AssignmentStatement>();
            if (assign->right->is<IR::MethodCallExpression>()) {
                auto mi = P4::MethodInstance::resolve(
                    assign->right->to<IR::MethodCallExpression>(), refMap, typeMap);
                if (mi->is<P4::ExternMethod>()) {
                    auto em = mi->to<P4::ExternMethod>();
                    if (em->method->name.name == v1model.ck16.get.name &&
                        em->type->name.name == v1model.ck16.name) {
                        BUG_CHECK(mi->expr->arguments->size() == 1,
                                  "%1%: Expected 1 argument", assign->right);
                        auto cksum = new Util::JsonObject();
                        cstring calcName = createCalculation("csum16", mi->expr->arguments->at(0),
                                                             calculations);
                        cksum->emplace("name", refMap->newName("cksum_"));
                        cksum->emplace("id", nextId("checksums"));
                        auto jleft = conv->convert(assign->left);
                        cksum->emplace("target", jleft->to<Util::JsonObject>()->get("value"));
                        cksum->emplace("type", "generic");
                        cksum->emplace("calculation", calcName);
                        checksums->append(cksum);
                        continue;
                    }
                }
            }
        }
        BUG("%1%: not handled yet", stat);
    }
}

void JsonConverter::addTypesAndInstances(const IR::Type_StructLike* type, bool meta,
                                         Util::JsonArray* headerTypes, Util::JsonArray* instances,
                                         std::set<cstring> &headerTypesCreated) {
    // TODO: this is wrong if the structs are more deeply nested.
    for (auto f : *type->getEnumerator()) {
        auto ft = typeMap->getType(f->type, true);
        if (ft->is<IR::Type_StructLike>()) {
            auto st = ft->to<IR::Type_StructLike>();
            if (headerTypesCreated.count(st->name))
                continue;
            auto json = typeToJson(st);
            headerTypes->append(json);
            headerTypesCreated.emplace(st->name);
        }
    }

    for (auto f : *type->getEnumerator()) {
        auto ft = typeMap->getType(f->type, true);
        if (ft->is<IR::Type_StructLike>()) {
            auto json = new Util::JsonObject();
            json->emplace("name", nameFromAnnotation(f->annotations, f->name));
            json->emplace("id", nextId("headers"));
            json->emplace("header_type", ft->to<IR::Type_StructLike>()->name.name);
            json->emplace("metadata", meta);
            instances->append(json);
        } else if (ft->is<IR::Type_Stack>()) {
            continue;
        } else {
            BUG("%1%: Unexpected header type", ft);
        }
    }
}

Util::IJson* JsonConverter::typeToJson(const IR::Type_StructLike* st) {
    auto result = new Util::JsonObject();
    result->emplace("name", nameFromAnnotation(st->annotations, st->name.name));
    result->emplace("id", nextId("header_types"));
    auto fields = mkArrayField(result, "fields");
    for (auto f : *st->getEnumerator()) {
        auto field = pushNewArray(fields);
        field->append(f->name.name);
        auto ftype = typeMap->getType(f->type, true);
        field->append(ftype->width_bits());
    }
    // must add padding
    unsigned width = st->width_bits();
    unsigned padding = width % 8;
    if (padding != 0) {
        cstring name = refMap->newName("_padding");
        auto field = pushNewArray(fields);
        field->append(name);
        field->append(8 - padding);
    }
    return result;
}

void JsonConverter::convertDeparserBody(const IR::Vector<IR::StatOrDecl>* body,
                                        Util::JsonArray* result) {
    conv->simpleExpressionsOnly = true;
    for (auto s : *body) {
        if (s->is<IR::BlockStatement>()) {
            convertDeparserBody(s->to<IR::BlockStatement>()->components, result);
            continue;
        } else if (s->is<IR::ReturnStatement>() || s->is<IR::ExitStatement>()) {
            break;
        } else if (s->is<IR::EmptyStatement>()) {
            continue;
        } else if (s->is<IR::MethodCallStatement>()) {
            auto mc = s->to<IR::MethodCallStatement>()->methodCall;
            auto mi = P4::MethodInstance::resolve(mc, refMap, typeMap);
            if (mi->is<P4::ExternMethod>()) {
                auto em = mi->to<P4::ExternMethod>();
                if (em->type->name.name == corelib.packetOut.name) {
                    if (em->method->name.name == corelib.packetOut.emit.name) {
                        BUG_CHECK(mc->arguments->size() == 1,
                                  "Expected exactly 1 argument for %1%", mc);
                        auto arg = mc->arguments->at(0);
                        auto type = typeMap->getType(arg, true);
                        if (type->is<IR::Type_Stack>()) {
                            int size = type->to<IR::Type_Stack>()->getSize();
                            for (int i=0; i < size; i++) {
                                auto j = conv->convert(arg);
                                auto e = j->to<Util::JsonObject>()->get("value");
                                BUG_CHECK(e->is<Util::JsonValue>(),
                                          "%1%: Expected a Json value", e->toString());
                                cstring ref = e->to<Util::JsonValue>()->getString();
                                ref += "[" + Util::toString(i) + "]";
                                result->append(ref);
                            }
                        } else {
                            auto j = conv->convert(arg);
                            result->append(j->to<Util::JsonObject>()->get("value"));
                        }
                    }
                    continue;
                }
            }
        }
        ::error("%1%: not supported with a deparser on this target", s);
    }
    conv->simpleExpressionsOnly = false;
}

Util::IJson* JsonConverter::convertDeparser(const IR::ControlContainer* ctrl) {
    auto result = new Util::JsonObject();
    result->emplace("name", "deparser");  // at least in simple_router this name is hardwired
    result->emplace("id", nextId("deparser"));
    auto order = mkArrayField(result, "order");
    convertDeparserBody(ctrl->body->components, order);
    return result;
}

Util::IJson* JsonConverter::toJson(const IR::ParserContainer* parser) {
    auto result = new Util::JsonObject();
    if (parser->stateful->size() != 0) {
        ::error("%1%: Stateful elements not yet supported in parser for this target", parser);
        return result;
    }
    result->emplace("name", "parser");  // at least in simple_router this name is hardwired
    result->emplace("id", nextId("parser"));
    result->emplace("init_state", IR::ParserState::start.name);
    auto states = mkArrayField(result, "parse_states");

    for (auto state : *parser->states) {
        auto json = toJson(state);
        if (json != nullptr)
            states->append(json);
    }
    return result;
}

Util::IJson* JsonConverter::convertParserStatement(const IR::StatOrDecl* stat) {
    auto result = new Util::JsonObject();
    auto params = mkArrayField(result, "parameters");
    if (stat->is<IR::AssignmentStatement>()) {
        auto assign = stat->to<IR::AssignmentStatement>();
        result->emplace("op", "set");
        auto l = conv->convert(assign->left);
        auto r = conv->convert(assign->right);
        params->append(l);
        params->append(r);
        return result;
    } else if (stat->is<IR::MethodCallStatement>()) {
        auto mce = stat->to<IR::MethodCallStatement>()->methodCall;
        auto minst = P4::MethodInstance::resolve(mce, refMap, typeMap);
        if (minst->is<P4::ExternMethod>()) {
            auto extmeth = minst->to<P4::ExternMethod>();
            if (extmeth->method->name.name == corelib.packetIn.extract.name) {
                result->emplace("op", "extract");
                if (mce->arguments->size() == 1) {
                    auto arg = mce->arguments->at(0);
                    auto param = new Util::JsonObject();
                    params->append(param);
                    cstring type;
                    Util::IJson* j = nullptr;

                    if (arg->is<IR::Member>()) {
                        auto mem = arg->to<IR::Member>();
                        auto baseType = typeMap->getType(mem->expr, true);
                        if (baseType->is<IR::Type_Stack>()) {
                            if (mem->member == IR::Type_Stack::next) {
                                type = "stack";
                                j = conv->convert(mem->expr);
                            } else {
                                BUG("%1%: unsupported", mem);
                            }
                        }
                    }
                    if (j == nullptr) {
                        type = "regular";
                        j = conv->convert(arg);
                    }
                    auto value = j->to<Util::JsonObject>()->get("value");
                    param->emplace("type", type);
                    param->emplace("value", value);
                    return result;
                }
            }
        }
    }
    ::error("%1%: not supported in parser on this target", stat);
    return result;
}

// Operates on a select keyset
void JsonConverter::convertSimpleKey(const IR::Expression* keySet,
                                         mpz_class& value, mpz_class& mask) const {
    if (keySet->is<IR::Mask>()) {
        auto mk = keySet->to<IR::Mask>();
        if (!mk->left->is<IR::Constant>()) {
            ::error("%1% must evaluate to a compile-time constant", mk->left);
            return;
        }
        if (!mk->right->is<IR::Constant>()) {
            ::error("%1% must evaluate to a compile-time constant", mk->right);
            return;
        }
        value = mk->left->to<IR::Constant>()->value;
        mask = mk->right->to<IR::Constant>()->value;
    } else if (keySet->is<IR::Constant>()) {
        value = keySet->to<IR::Constant>()->value;
        mask = -1;
    } else {
        ::error("%1% must evaluate to a compile-time constant", keySet);
        value = 0;
        mask = 0;
    }
}

unsigned JsonConverter::combine(const IR::Expression* keySet,
                            const IR::ListExpression* select,
                            mpz_class& value, mpz_class& mask) const {
    // From the BMv2 spec: For values and masks, make sure that you
    // use the correct format. They need to be the concatenation (in
    // the right order) of all byte padded fields (padded with 0
    // bits). For example, if the transition key consists of a 12-bit
    // field and a 2-bit field, each value will need to have 3 bytes
    // (2 for the first field, 1 for the second one). If the
    // transition value is 0xaba, 0x3, the value attribute will be set
    // to 0x0aba03.
    // Return width in bytes
    value = 0;
    mask = 0;
    unsigned totalWidth = 0;
    if (keySet->is<IR::DefaultExpression>()) {
        return totalWidth;
    } else if (keySet->is<IR::ListExpression>()) {
        auto le = keySet->to<IR::ListExpression>();
        BUG_CHECK(le->components->size() == select->components->size(),
                  "%1%: mismatched select", select);
        unsigned index = 0;

        bool noMask = true;
        for (auto it = select->components->begin();
             it != select->components->end(); ++it) {
            auto e = *it;
            auto keyElement = le->components->at(index);

            auto type = typeMap->getType(e, true);
            int width = type->width_bits();
            BUG_CHECK(width > 0, "%1%: unknown width", e);

            mpz_class key_value, mask_value;
            convertSimpleKey(keyElement, key_value, mask_value);
            unsigned w = 8 * ROUNDUP(width, 8);
            totalWidth += ROUNDUP(width, 8);
            value = Util::shift_left(value, w) + key_value;
            if (mask_value != -1) {
                mask = Util::shift_left(mask, w) + mask_value;
                noMask = false;
            }
            LOG1("Shifting " << " into key " << key_value << " &&& " << mask_value <<
                 " result is " << value << " &&& " << mask);
            index++;
        }

        if (noMask)
            mask = -1;
        return totalWidth;
    } else {
        BUG_CHECK(select->components->size() == 1, "%1%: mismatched select/label", select);
        convertSimpleKey(keySet, value, mask);
        auto type = typeMap->getType(select->components->at(0), true);
        return type->width_bits() / 8;
    }
}

static Util::IJson* stateName(IR::ID state) {
    if (state.name == IR::ParserState::accept) {
        return Util::JsonValue::null;
    } else if (state.name == IR::ParserState::reject) {
        ::warning("Explicit transition to %1% not supported on this target", state);
        return Util::JsonValue::null;
    } else {
        return new Util::JsonValue(state.name);
    }
}

Util::IJson* JsonConverter::toJson(const IR::ParserState* state) {
    if (state->name == IR::ParserState::reject || state->name == IR::ParserState::accept)
        return nullptr;

    auto result = new Util::JsonObject();
    result->emplace("name", nameFromAnnotation(state->annotations, state->name.name));
    result->emplace("id", nextId("parse_states"));
    auto operations = mkArrayField(result, "parser_ops");
    for (auto s : *state->components) {
        auto j = convertParserStatement(s);
        operations->append(j);
    }

    Util::IJson* key;
    auto transitions = mkArrayField(result, "transitions");
    if (state->selectExpression != nullptr) {
        if (state->selectExpression->is<IR::SelectExpression>()) {
            auto se = state->selectExpression->to<IR::SelectExpression>();
            key = conv->convert(se->select);
            for (auto sc : *se->selectCases) {
                auto trans = new Util::JsonObject();
                mpz_class value, mask;
                unsigned bytes = combine(sc->keyset, se->select, value, mask);
                if (mask == 0) {
                    trans->emplace("value", "default");
                    trans->emplace("mask", Util::JsonValue::null);
                    trans->emplace("next_state", stateName(sc->state->path->name));
                } else {
                    trans->emplace("value", stringRepr(value, bytes));
                    if (mask == -1)
                        trans->emplace("mask", Util::JsonValue::null);
                    else
                        trans->emplace("mask", stringRepr(mask, bytes));
                    trans->emplace("next_state", stateName(sc->state->path->name));
                }
                transitions->append(trans);
            }
        } else if (state->selectExpression->is<IR::PathExpression>()) {
            auto pe = state->selectExpression->to<IR::PathExpression>();
            key = new Util::JsonArray();
            auto trans = new Util::JsonObject();
            trans->emplace("value", "default");
            trans->emplace("mask", Util::JsonValue::null);
            trans->emplace("next_state", stateName(pe->path->name));
            transitions->append(trans);
        } else {
            BUG("%1%: unexpected selectExpression", state->selectExpression);
        }
    } else {
        key = new Util::JsonArray();
        auto trans = new Util::JsonObject();
        trans->emplace("value", "default");
        trans->emplace("mask", Util::JsonValue::null);
        trans->emplace("next_state", Util::JsonValue::null);
        transitions->append(trans);
    }
    result->emplace("transition_key", key);
    return result;
}

}  // namespace BMV2