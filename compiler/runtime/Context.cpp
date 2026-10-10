#include "map"
#include "unordered_map"

#include "../runtime/Values.cpp"
#include "../utils/Strings.cpp"

#pragma once

enum ContextType {
    Global,
    NamespaceT,
    StructT,
    FunctionT,
};

typedef String CtxObjectIdentifier;

struct FunctionContainer;   // определён в Functions.cpp
struct Node;                // forward-decl из core/Nodes (для ref-алиасов)
struct Context;             // определён ниже (для ref_alias_ctx)

struct ContextObject {
    struct Modifiers {
        bool Strict = false;
        bool Const = false;
        bool Private = false;
        bool Static = false;
        bool Final = false;
        bool Ref = false;
        // Порядок модификаторов в объявлении (`final strict let a …`) —
        // для hover/автодополнения, где важно исходное написание.
        std::vector<String> order;
    };

    CtxObjectIdentifier identifier;
    Value value;
    Type wait_type = Ty::Null;
    Modifiers modifiers;
    uint64_t addr = 0;
    // настоящее объявление типа (встроенный `i32`, generic-bound T и т.п.),
    // а не переменная, чьим значением оказался тип (`let a: auto = i32;`)
    bool is_type_decl = false;
    // ref-алиас (`ref let r = y;`): lvalue-узел цели. Чтение/запись этой
    // переменной переадресуются цели. Nullptr — обычная переменная.
    Node* ref_alias_node = nullptr;
    // Контекст, в котором нужно резолвить ref_alias_node (тот же, где был
    // объявлен алиас). Nullptr = резолвить в контексте текущего доступа.
    Context* ref_alias_ctx = nullptr;

    ContextObject(CtxObjectIdentifier identifier, Value value) :
        identifier(identifier), value(value), modifiers{0, 0, 0, 0, 0, 0} {}

    ContextObject(CtxObjectIdentifier identifier, Value value, Type wait_type) :
        identifier(identifier), value(value), wait_type(wait_type), modifiers{0, 0, 0, 0, 0, 0} {}
};



struct Context {
    ContextType type;
    Context* parent = nullptr;
    std::map<String, ContextObject> variables;
    std::map<String, ContextObject> namespaces;



    Context(ContextType type = Global) : type(type) {}

    bool has_variable(CtxObjectIdentifier identifier) const {
        if (variables.find(identifier) != variables.end()) return true;
        if (parent) return parent->has_variable(identifier);
        return false;
    }

    ContextObject& get_variable(CtxObjectIdentifier identifier) {
        auto it = variables.find(identifier);
        if (it != variables.end()) {
            return it->second;
        }
        if (parent) return parent->get_variable(identifier);
        throw std::runtime_error("Variable not found: " + identifier.to_utf8());
    }

    bool has_variable_local(CtxObjectIdentifier identifier) const {
        return variables.find(identifier) != variables.end();
    }

    ContextObject& get_variable_local(CtxObjectIdentifier identifier) {
        auto it = variables.find(identifier);
        if (it != variables.end()) {
            return it->second;
        }
        throw std::runtime_error("Variable not found (local): " + identifier.to_utf8());
    }

    void add_variable(CtxObjectIdentifier identifier, ContextObject object) {
        variables.insert_or_assign(identifier, std::move(object));
    }

    // --- контейнеры функций (перегрузки) ---
    void add_function_container(CtxObjectIdentifier identifier, FunctionContainer container);
    bool has_function_container(CtxObjectIdentifier identifier) const;
    bool has_function_container_local(CtxObjectIdentifier identifier) const;
    FunctionContainer& get_function_container(CtxObjectIdentifier identifier);
    FunctionContainer& get_function_container_local(CtxObjectIdentifier identifier);
};

struct Namespace {
    Context context;
    Namespace() : context(Context(ContextType::NamespaceT)) {}

    // Копия namespace (возврат из функции, присваивание): функции, объявленные
    // внутри литерала, держат context.parent = &исходный контекст (стек/локаль).
    // После побайтового копирования эти указатели становятся висячими (UB,
    // зависания анализатора при вызове членов). Копия перепривязывает их на себя.
    // (реализация в Functions.cpp — там FunctionContainer уже полный тип)
    Namespace(const Namespace& other);
    Namespace& operator=(const Namespace& other);
    void rebind(Context* src, Context* dst);
};

Value new_namespace() {
    return Value(Ty::Namespace, Namespace());
}


// Адресное пространство для value/var-ячеек: отделено от арены `new`,
// чтобы адреса new-ячеек (1,2,3,…) совпадали с рантайм-ареной и не зависели
// от числа объявленных переменных (`*(p+1)` = следующая new-ячейка).
typedef uint64_t VirtualAddress;
enum : VirtualAddress { VIRTUAL_HEAP_HIGH_BASE = 0x1000000000000000ULL };
enum : VirtualAddress { VIRTUAL_HEAP_SLOT_BASE = 0x2000000000000000ULL };

struct VirtualHeap {
    VirtualAddress _address;   // счётчик арены `new` (низкие адреса 1,2,3,…)
    VirtualAddress _high;      // счётчик value/var-ячеек (высокие адреса)
    VirtualAddress _slot;      // счётчик адресов указателей на поля (`&obj.field`)
    std::unordered_map<VirtualAddress, ContextObject*> heap;

    VirtualHeap() : _address(0), _high(VIRTUAL_HEAP_HIGH_BASE), _slot(VIRTUAL_HEAP_SLOT_BASE) {}

    VirtualAddress allocate(ContextObject* object) {
        VirtualAddress address = ++_address;
        heap.insert_or_assign(address, object);
        return address;
    }

    VirtualAddress allocate_high(ContextObject* object) {
        VirtualAddress address = ++_high;
        heap.insert_or_assign(address, object);
        return address;
    }

    // Адрес для `&obj.field`: уникальный и намеренно НЕ регистрируется в heap.
    // Такой адрес не является ContextObject*, поэтому heap.contains()/heap.get()
    // не могут выдать посторонний объект: потребители указателей на поля
    // опознают его по field_slots, а всё остальное увидит «нет такой ячейки».
    VirtualAddress allocate_slot() {
        return ++_slot;
    }

    bool contains(VirtualAddress address) {
        return heap.find(address) != heap.end();
    }

    ContextObject* get(VirtualAddress address) {
        return heap[address];
    }
};


struct Pointer {
    Type pointee;
    uint64_t addr;
};

Value new_pointer(const Type& pointee, uint64_t addr) {
    return Value(*pointee, Pointer{pointee, addr});
}
