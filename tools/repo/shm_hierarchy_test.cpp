// Exercise full and scoped hierarchy import against a controlled private ABI
// tree. This is not SDK qualification: real-runtime conformance is separate.
#include "../../native/shm.cpp"
#include <cassert>
#include <cstdarg>
#include <fstream>
#include <iostream>

namespace {
// One tagged handle space: objects (scopes, variables), types, iterators.
struct Base { int tag; };
struct Type : Base { int kind; };
struct Obj : Base {
    std::string name, full;
    int kind = 6;               // scope kind; ignored for variables
    Type *type = nullptr;       // variables only
    std::vector<Obj *> scopes;  // 0x2ae: scopes, also some variables
    std::vector<Obj *> vars;    // 0x2be
    Obj *definition = nullptr;
};
struct Iter : Base { const std::vector<Obj *> *list; size_t index; };
Type bits{{1}, 2}, composite{{1}, 8};
std::vector<std::unique_ptr<Obj>> pool;
Obj root_object;
size_t live_iterators = 0, type_queries = 0;
std::unordered_map<const Obj *, size_t> iterations;
std::unordered_set<const Obj *> typed;

Obj *make(Obj &parent, const std::string &name, bool variable, bool in_scopes = true, int kind = 6) {
    pool.push_back(std::make_unique<Obj>());
    auto &o = *pool.back();
    o.tag = 0; o.name = name; o.kind = kind;
    o.full = parent.full.empty() ? name : parent.full + "." + name;
    if (variable) o.type = &bits;
    else {
        pool.push_back(std::make_unique<Obj>());
        o.definition = pool.back().get();
        o.definition->tag = 0; o.definition->name = name + "_def";
    }
    (in_scopes ? parent.scopes : parent.vars).push_back(&o);
    return &o;
}
void scope_body(Obj &scope) {
    make(scope, "v0", true);           // a variable in the 0x2ae enumeration
    make(scope, "w0", true, false);
    make(scope, "w1", true, false);
    auto *s = make(scope, "s", true, false);
    s->type = &composite;               // composite variable with two fields
    make(*s, "f0", true); make(*s, "f1", true);
}
// top { v0, a { x { ... } b { ... } }, c[0..2] { ... } (transparent array), d { ... } }
void build() {
    root_object.tag = 0;
    auto *top = make(root_object, "top", false);
    scope_body(*top);
    auto *a = make(*top, "a", false);
    scope_body(*a);
    auto *x = make(*a, "x", false); scope_body(*x);
    auto *b = make(*a, "b", false); scope_body(*b);
    auto *bb = make(*b, "deep", false); scope_body(*bb);
    auto *c = make(*top, "c", false, true, 10);
    c->full = top->full;  // transparent: elements are named c[i] in top
    for (int i = 0; i < 3; ++i) {
        auto *element = make(*c, std::to_string(i), false);
        element->full = top->full + ".c[" + std::to_string(i) + "]";
        scope_body(*element);
    }
    auto *d = make(*top, "d", false); scope_body(*d);
    make(*top, "a2", false);            // shares a prefix with a selected name
}
Obj &object(SstHandle handle) {
    auto *base = reinterpret_cast<Base *>(handle);
    assert(base && base->tag == 0);
    return *static_cast<Obj *>(base);
}
}
extern "C" {
int sst2Init() { return 1; }
int drOpenFile(const char *, SstHandle *out, int (*)(void *, int), void *) {
    *out = reinterpret_cast<SstHandle>(&root_object); return 0;
}
int drClose(SstHandle) { return 0; }
int da_get(int property, SstHandle handle) {
    if (property == 0x12) return 0;
    if (property == 0x173) return handle ? reinterpret_cast<Type *>(handle)->kind : -1;
    auto &o = object(handle);
    if (property == 0x113) return o.kind;
    if (property == 0x143) return 1;
    if (property == 0x122 || property == 0x103) return 0;
    std::cerr << "unexpected da_get " << std::hex << property << '\n'; std::abort();
}
int da_get_int(int property, SstHandle handle, int *out) {
    if (property == 0x194) { *out = -9; return 0; }
    if (property == 0x3d4 && reinterpret_cast<Type *>(handle) == &bits) { *out = 1; return 0; }
    if (property == 0x164) { *out = 2; return 0; }
    return -1;
}
int da_get_real(int, SstHandle, double *) { std::abort(); }
const char *da_get_sstr(int property, SstHandle handle) {
    assert(property == 0x10a);
    return object(handle).name.c_str();
}
const char *da_get_sstr_pl(int, SstHandle, ...) { std::abort(); }
SstHandle da_get_xref(int property, SstHandle handle) { assert(property == 0x13d); return handle; }
int da_get_xvalseq(int property, SstHandle, SstTime *out) {
    *out = {property == 0x106 ? 0ULL : 100ULL, 0}; return 0;
}
SstHandle da_handle(int property, SstHandle handle) {
    if (property == 0x1ae) return 1;
    if (property == 0x1de) return 0;    // no index type: no HDL range
    auto &o = object(handle);
    if (property == 0x1be) { if (o.type) { ++type_queries; typed.insert(&o); } return reinterpret_cast<SstHandle>(o.type); }
    if (property == 0x14e) return reinterpret_cast<SstHandle>(o.definition);
    std::cerr << "unexpected da_handle " << std::hex << property << '\n'; std::abort();
}
SstHandle da_handle_pl(int, SstHandle, ...) { std::abort(); }
SstHandle da_iterate(int property, SstHandle handle) {
    if (property == 0x1fe) return 0;    // no enum elements
    auto &o = object(handle);
    ++iterations[&o];
    ++live_iterators;
    auto *iterator = new Iter{{2}, property == 0x2ae ? &o.scopes : &o.vars, 0};
    return reinterpret_cast<SstHandle>(iterator);
}
SstHandle da_scan(SstHandle handle) {
    auto *iterator = reinterpret_cast<Iter *>(handle);
    assert(iterator->tag == 2);
    if (iterator->index < iterator->list->size())
        return reinterpret_cast<SstHandle>((*iterator->list)[iterator->index++]);
    delete iterator; --live_iterators;
    return 0;
}
void da_delete(SstHandle handle) {
    auto *iterator = reinterpret_cast<Iter *>(handle);
    assert(iterator->tag == 2);
    delete iterator; --live_iterators;
}
}

namespace {
std::string directory;
struct Entry { std::string path; int entry; std::uint32_t encoding; std::string kind, definition; };
std::vector<Entry> entries(const ondas_shm &reader) {
    std::vector<Entry> out;
    std::vector<std::string> stack;
    for (const auto &d : reader.declarations) {
        const auto prefix = stack.empty() ? std::string() : stack.back() + ".";
        if (d.descriptor.entry == 1) { stack.pop_back(); continue; }
        out.push_back({prefix + d.name, int(d.descriptor.entry), d.descriptor.encoding, d.kind, d.definition});
        if (d.descriptor.entry == 0) stack.push_back(prefix + d.name);
    }
    assert(stack.empty());
    return out;
}
bool under(const std::string &path, const std::string &scope) {
    return path == scope || path.compare(0, scope.size() + 1, scope + ".") == 0;
}
std::unique_ptr<ondas_shm> open_scoped(std::vector<std::vector<std::string>> paths, std::string *failure = nullptr) {
    std::vector<const char *> components;
    std::vector<size_t> lengths;
    for (const auto &path : paths) {
        lengths.push_back(path.size());
        for (const auto &c : path) components.push_back(c.c_str());
    }
    ondas_shm *raw = nullptr;
    char error[256]{};
    int os_error = -1;
    const int code = ondas_shm_open_scoped(directory.c_str(), components.data(), lengths.data(),
                                           paths.size(), &raw, error, sizeof(error), &os_error);
    assert(os_error == 0 && live_iterators == 0);
    if (failure) { assert(code == -1 && !raw); *failure = error; return nullptr; }
    if (code) { std::cerr << error << '\n'; std::abort(); }
    return std::unique_ptr<ondas_shm>(raw);
}
std::unique_ptr<ondas_shm> open_full() {
    ondas_shm *raw = nullptr;
    char error[256]{};
    int os_error = -1;
    assert(ondas_shm_open(directory.c_str(), 0, &raw, error, sizeof(error), &os_error) == 0);
    return std::unique_ptr<ondas_shm>(raw);
}
// Every imported variable route must resolve the object at the same full path.
void check_routes(ondas_shm &reader) {
    const auto view = entries(reader);
    std::vector<std::uint64_t> ids;
    std::vector<std::string> expected;
    size_t row = 0;
    for (size_t i = 0; i < reader.declarations.size(); ++i) {
        const auto &d = reader.declarations[i];
        if (d.descriptor.entry == 1) continue;
        if (d.descriptor.entry == 2) {
            assert(d.descriptor.id == i);  // dense identities in the imported vector
            ids.push_back(i);
            expected.push_back(view[row].path);
        }
        ++row;
    }
    ondas_shm_work work{};
    const auto resolved = selected_declarations(reinterpret_cast<SstHandle>(&root_object), ids.data(), ids.size(),
                                                reader.declarations, reader.source_nodes, work);
    assert(resolved.size() == ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        // Composite fields are named parent[field] in Ondas and parent.field here.
        auto want = expected[i];
        for (auto open = want.find('[', want.rfind('.')); open != std::string::npos; open = want.find('[', open)) {
            want[open] = '.';
            want.erase(want.find(']', open), 1);
        }
        auto &got = object(resolved[i].variable).full;
        if (got != want) { std::cerr << got << " != " << want << '\n'; std::abort(); }
    }
    assert(live_iterators == 0);
}
void full_import_is_unchanged() {
    auto full = open_full();
    auto scoped = open_scoped({});
    const auto a = entries(*full), b = entries(*scoped);
    assert(a.size() == b.size() && full->source_nodes.size() == scoped->source_nodes.size());
    for (size_t i = 0; i < a.size(); ++i) assert(a[i].path == b[i].path && a[i].entry == b[i].entry);
    check_routes(*full);
    std::cout << "full import: entries=" << a.size() << " routes=" << full->source_nodes.size() << '\n';
}
void scoped_import_matches_full_subtrees() {
    auto full = open_full();
    const auto all = entries(*full);
    const auto before = type_queries;
    iterations.clear();
    typed.clear();
    auto scoped = open_scoped({{"top", "a", "b"}, {"top", "c[1]"}});
    const auto queries = type_queries - before;
    const auto some = entries(*scoped);
    const std::vector<std::string> selected{"top.a.b", "top.c[1]"};
    const std::vector<std::string> ancestors{"top", "top.a"};
    // Selected subtrees equal the full import's entries, in order.
    std::vector<Entry> expected;
    for (const auto &e : all) {
        bool keep = std::count(ancestors.begin(), ancestors.end(), e.path) > 0;
        for (const auto &s : selected) keep = keep || under(e.path, s);
        if (keep) expected.push_back(e);
    }
    assert(some.size() == expected.size());
    for (size_t i = 0; i < some.size(); ++i) {
        assert(some[i].path == expected[i].path && some[i].entry == expected[i].entry &&
               some[i].encoding == expected[i].encoding && some[i].kind == expected[i].kind &&
               some[i].definition == expected[i].definition);
    }
    // Ancestors have no variables; unselected subtrees are absent.
    for (const auto &e : some) {
        if (e.entry == 2) {
            bool inside = false;
            for (const auto &s : selected) inside = inside || under(e.path, s);
            assert(inside);
        }
        assert(!under(e.path, "top.d") && !under(e.path, "top.a.x") && !under(e.path, "top.c[0]") && e.path != "top.a2");
    }
    // No type query for skipped names; unselected scopes are not enumerated.
    for (const auto &object : pool) {
        if (object->full == "top.d" || object->full == "top.a.x" || object->full == "top.c[0]" ||
            object->full == "top.c[2]" || object->full == "top.a2")
            assert(!iterations.count(object.get()));
        if (object->full == "top" || object->full == "top.a") assert(iterations[object.get()] == 1);  // 0x2ae only
    }
    // Variables outside the selected subtrees get no type query.
    for (const auto *o : typed) {
        bool inside = false;
        for (const auto &s : selected) inside = inside || under(o->full, s);
        assert(inside);
    }
    check_routes(*scoped);
    std::cout << "scoped import: entries=" << some.size() << " of " << all.size()
              << " routes=" << scoped->source_nodes.size() << " type_queries=" << queries << '\n';
}
void nested_and_missing_selections() {
    // A selection below another selection adds nothing.
    auto a = open_scoped({{"top", "a"}, {"top", "a", "b", "deep"}});
    auto b = open_scoped({{"top", "a"}});
    const auto x = entries(*a), y = entries(*b);
    assert(x.size() == y.size());
    for (size_t i = 0; i < x.size(); ++i) assert(x[i].path == y[i].path);
    check_routes(*a);
    std::string failure;
    open_scoped({{"top", "a"}, {"top", "nope"}}, &failure);
    assert(failure == "SHM scope not found: top.nope");
    open_scoped({{"top", "v0"}}, &failure);  // a variable is not a scope
    assert(failure == "SHM scope not found: top.v0");
}
std::vector<std::pair<std::string, int>> list(std::vector<std::string> path) {
    std::vector<const char *> components;
    for (const auto &c : path) components.push_back(c.c_str());
    ondas_shm *raw = nullptr;
    char error[256]{};
    int os_error = -1;
    const int code = ondas_shm_list(directory.c_str(), components.data(), components.size(),
                                    &raw, error, sizeof(error), &os_error);
    std::vector<std::pair<std::string, int>> out;
    if (code) { out.push_back({error, -1}); return out; }
    for (size_t i = 0; i < ondas_shm_child_count(raw); ++i) {
        ondas_shm_child child{};
        ondas_shm_child_at(raw, i, &child);
        out.push_back({child.name, child.is_scope});
    }
    ondas_shm_close(raw);
    assert(live_iterators == 0);
    return out;
}
void listing() {
    using L = std::vector<std::pair<std::string, int>>;
    assert(list({}) == (L{{"top", 1}}));
    const auto before = type_queries;
    assert(list({"top"}) == (L{{"v0", 0}, {"a", 1}, {"c[0]", 1}, {"c[1]", 1}, {"c[2]", 1}, {"d", 1}, {"a2", 1},
                               {"w0", 0}, {"w1", 0}, {"s", 0}}));
    assert(type_queries - before == 1);  // only the 0x2ae variable is classified
    assert(list({"top", "c[1]"}) == (L{{"v0", 0}, {"w0", 0}, {"w1", 0}, {"s", 0}}));
    assert(list({"top", "zz"}) == (L{{"SHM scope not found: top.zz", -1}}));
    std::cout << "listing: ok\n";
}
}
int main(int argc, char **argv) {
    assert(argc == 2);
    directory = argv[1];
    std::ofstream(directory + "/w.dsn").put('\n');
    std::ofstream(directory + "/w.trn").put('\n');
    build();
    full_import_is_unchanged();
    scoped_import_matches_full_subtrees();
    nested_and_missing_selections();
    listing();
    assert(live_iterators == 0);
}
