#include "./parse_tree.h"
#include "./memory.h"
#include "./vm.h"

ZymParseTree* parse_tree_new(VM* vm, AstResult ast, ZymFileId file_id,
                             TriviaBuffer* trivia) {
    ZymParseTree* tree = ALLOCATE(vm, ZymParseTree, 1);
    tree->ast = ast;
    tree->file_id = file_id;
    tree->trivia = trivia;
    return tree;
}

void parse_tree_free(VM* vm, ZymParseTree* tree) {
    if (tree == NULL) return;
    if (tree->ast.statements != NULL) {
        for (int i = 0; tree->ast.statements[i] != NULL; i++) {
            free_stmt(vm, tree->ast.statements[i]);
        }
        FREE_ARRAY(vm, Stmt*, tree->ast.statements, tree->ast.capacity);
    }
    if (tree->trivia != NULL) {
        trivia_free(vm, tree->trivia);
        FREE(vm, TriviaBuffer, tree->trivia);
    }
    FREE(vm, ZymParseTree, tree);
}
