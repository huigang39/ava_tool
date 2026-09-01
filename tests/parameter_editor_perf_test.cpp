#include "core/parameter_file.hpp"
#include "core/parameter_tree.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace parameter_json;

template <class F>
static void
timed(const char *name, F &&operation)
{
        const auto start = std::chrono::steady_clock::now();
        operation();
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << name << ": " << elapsed << " ms\n";
}

static Document
fixture(size_t count)
{
        Document doc;
        doc.name = "fixture";
        // Reverse insertion exercises numeric array ordering independently of
        // the JSON field order, including [9], [10] and [100]
        for (const auto *section : componentSections)
                for (size_t i = count; i-- > 0;) {
                        Field f;
                        f.row.path  = std::string(section) + ".values[" + std::to_string(i) + "]";
                        f.row.type  = "uint32_t";
                        f.row.val = std::to_string(i);
                        doc.fields.push_back(std::move(f));
                }
        return doc;
}

int
main()
{
        constexpr size_t width = 4000;
        auto             doc   = fixture(width);
        const auto       text  = format(doc);
        TreeNode         root;
        timed("tree_12000_fields", [&] { root = tree(doc); });
        assert(root.children.size() == 3);
        for (size_t section = 0; section < 3; ++section) {
                const auto &array = root.children[section].children.at(0);
                assert(array.children.size() == width);
                for (size_t i = 0; i < width; ++i) {
                        const auto &node = array.children[i];
                        assert(node.name == "[" + std::to_string(i) + "]");
                        assert(doc.fields.at(node.field).row.path == node.path);
                        assert(doc.fields.at(node.field).row.val == std::to_string(i));
                }
        }
        FileEditor editor;
        editor.load(text);
        editor.refreshNames();
        const auto *stableTree = &editor.fieldTree();
        timed("unchanged_name_and_tree_100_frames", [&] {
                for (int frame = 0; frame < 100; ++frame) {
                        editor.refreshNames();
                        assert(&editor.fieldTree() == stableTree);
                }
        });
        editor.document.fields[0].row.val = "99999";
        editor.dirty                        = true;
        editor.refreshNames();
        assert(editor.document.name == "fixture*/fixture/fixture");
        assert(&editor.fieldTree() == stableTree);
        std::string saved;
        timed("save_12000_fields", [&] { saved = editor.save(); });
        assert(parse(saved).fields[0].row.val == "99999");
        auto preset                    = doc;
        preset.name                    = "replacement";
        preset.fields[width].row.val = "77777";
        const auto incoming            = format(preset);
        timed("replace_component_12000_fields", [&] { editor.replaceComponent("hw", incoming); });
        assert(editor.document.fields[0].row.val == "99999");
        assert(editor.document.fields[width].row.val == "77777");
        assert(editor.document.fields[width * 2].row.val == doc.fields[width * 2].row.val);
        assert(editor.document.name == "fixture*/replacement/fixture");
        editor.reset();
        assert(editor.document.fields[0].row.val == doc.fields[0].row.val && !editor.dirty);
        editor.refreshNames();
        assert(editor.document.name == "fixture");
        auto copy = editor;
        assert(&copy.fieldTree() == &editor.fieldTree());
        copy.document.fields[0].row.path = "mech.added";
        const auto &updated              = copy.fieldTree();
        assert(&updated != &editor.fieldTree());
        assert(updated.children[0].children[0].name == "added");
        assert(editor.fieldTree().children[0].children[0].name == "values");
        copy.document.fields.clear();
        assert(copy.fieldTree().children.empty());

        // Cache validity must include input metadata and defaults, even when a
        // caller edits those directly instead of going through UI setters
        FileEditor names;
        names.load(format(fixture(1)));
        names.refreshNames();
        names.document.fields[0].presetValue = "1";
        names.refreshNames();
        assert(names.document.name == "fixture*/fixture/fixture");
        names.document.fields[0].row.val = "1";
        names.refreshNames();
        assert(names.document.name == "fixture/fixture/fixture");
        names.document.fields[0].enums     = {{"ONE", "1"}};
        names.document.fields[0].row.val = "ONE";
        names.refreshNames();
        assert(names.document.name == "fixture/fixture/fixture");
        names.document.fields[0].enums["ONE"] = "2";
        names.refreshNames();
        assert(names.document.name == "fixture*/fixture/fixture");

        // Saving must reject detached/truncated metadata instead of leaving an
        // old invisible field in the written file
        editor.document.fields.pop_back();
        bool rejected = false;
        try {
                editor.save();
        } catch (const std::exception &) {
                rejected = true;
        }
        assert(rejected);
        std::cout << "PASS: wide numeric arrays, component isolation and reset baseline\n";
        std::cout << "PASS: direct name/default/enum edits, shared cache isolation and stale metadata rejection\n";
}
