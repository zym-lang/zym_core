#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./gc.h"
#include "./memory.h"
#include "./vm.h"
#include "./object.h"
#include "./value.h"
#include "./table.h"
#include "./chunk.h"

static void markRoots(VM* vm);
static void traceReferences(VM* vm);
static void blackenObject(VM* vm, Obj* object);
static void sweep(VM* vm);
static void markChunk(VM* vm, Chunk* chunk);

// Protect objects during multi-step construction

void pushTempRoot(VM* vm, Obj* object) {
    if (object == NULL) return;

    if (vm->temp_root_count >= vm->temp_root_capacity) {
        int old_capacity = vm->temp_root_capacity;
        vm->temp_root_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        vm->temp_roots = (Obj**)realloc(vm->temp_roots, sizeof(Obj*) * vm->temp_root_capacity);
        if (vm->temp_roots == NULL) {
            fprintf(stderr, "Fatal: Out of memory for temp roots\n");
            exit(1);
        }
    }

    vm->temp_roots[vm->temp_root_count++] = object;
}

void popTempRoot(VM* vm) {
    if (vm->temp_root_count > 0) {
        vm->temp_root_count--;
    }
}

void markValue(VM* vm, Value value) {
    if (IS_OBJ(value)) {
        markObject(vm, AS_OBJ(value));
    }
}

void markObject(VM* vm, Obj* object) {
    if (object == NULL) {
        #ifdef GC_DEBUG_FULL
        printf("markObject called with NULL\n");
        fflush(stdout);
        #endif
        return;
    }
    if (object->type < 0 || object->type > 20) {
        //#ifdef GC_DEBUG_FULL
        printf("WARNING: markObject called with invalid object %p [type=%d] - skipping\n",
               (void*)object, object->type);
        fflush(stdout);
        //#endif
        return;
    }

    if (object->is_marked) {
        #ifdef GC_DEBUG_FULL
        printf("%p already marked [type=%d]\n", (void*)object, object->type);
        fflush(stdout);
        #endif
        return;
    }

    #ifdef GC_DEBUG_FULL
    printf("%p mark [type=%d] ", (void*)object, object->type);
    fflush(stdout);
    printValue(vm, OBJ_VAL(object));
    printf("\n");
    fflush(stdout);
    #endif

    object->is_marked = true;
    if (vm->gray_capacity < vm->gray_count + 1) {
        #ifdef GC_DEBUG_FULL
        printf("Gray stack needs to grow: count=%d, capacity=%d, old_ptr=%p\n",
               vm->gray_count, vm->gray_capacity, (void*)vm->gray_stack);
        fflush(stdout);
        #endif

        int old_capacity = vm->gray_capacity;
        vm->gray_capacity = vm->gray_capacity < 8 ? 8 : vm->gray_capacity * 2;

        #ifdef GC_DEBUG_FULL
        printf("Growing gray stack from %d to %d\n", old_capacity, vm->gray_capacity);
        fflush(stdout);
        #endif

        vm->gray_stack = (Obj**)realloc(vm->gray_stack, sizeof(Obj*) * vm->gray_capacity);

        #ifdef GC_DEBUG_FULL
        printf("Gray stack reallocated: new_ptr=%p\n", (void*)vm->gray_stack);
        fflush(stdout);
        #endif

        if (vm->gray_stack == NULL) {
            fprintf(stderr, "Fatal: Out of memory during GC marking\n");
            exit(1);
        }
    }

    #ifdef GC_DEBUG_FULL
    printf("Adding %p to gray_stack[%d]\n", (void*)object, vm->gray_count);
    fflush(stdout);
    #endif

    vm->gray_stack[vm->gray_count++] = object;

    #ifdef GC_DEBUG_FULL
    printf("%p added to gray stack (count=%d)\n", (void*)object, vm->gray_count);
    fflush(stdout);
    #endif
}

void markTable(VM* vm, Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL) {
            #ifdef GC_DEBUG_FULL
            if (entry->key->obj.type < 0 || entry->key->obj.type > 20) {
                printf("Table entry[%d] has invalid key: %p [type=%d]\n",
                       i, (void*)entry->key, entry->key->obj.type);
                fflush(stdout);
            }
            #endif
            markObject(vm, (Obj*)entry->key);
            markValue(vm, entry->value);
        }
    }
}

static void markChunk(VM* vm, Chunk* chunk) {
    if (chunk == NULL) return;
    #ifdef GC_DEBUG_FULL
    printf("  markChunk %p: marking %d constants\n", (void*)chunk, chunk->constants.count);
    fflush(stdout);
    #endif
    for (int i = 0; i < chunk->constants.count; i++) {
        #ifdef GC_DEBUG_FULL
        if (IS_OBJ(chunk->constants.values[i]) && IS_FUNCTION(chunk->constants.values[i])) {
            printf("    Constant[%d]: function %p\n", i, (void*)AS_OBJ(chunk->constants.values[i]));
            fflush(stdout);
        }
        #endif
        markValue(vm, chunk->constants.values[i]);
    }
}

static void markRoots(VM* vm) {
    #ifdef GC_DEBUG_FULL
    printf("Marking %d temporary roots\n", vm->temp_root_count);
    fflush(stdout);
    #endif
    for (int i = 0; i < vm->temp_root_count; i++) {
        markObject(vm, vm->temp_roots[i]);
    }

    #ifdef GC_DEBUG_FULL
    printf("Marking %d stack values\n", vm->stack_top);
    fflush(stdout);
    #endif
    for (int i = 0; i < vm->stack_top; i++) {
        markValue(vm, vm->stack[i]);
    }

    #ifdef GC_DEBUG_FULL
    printf("Marking global variables\n");
    fflush(stdout);
    #endif
    markTable(vm, &vm->globals);

    #ifdef GC_DEBUG_FULL
    printf("Marking global slots array\n");
    fflush(stdout);
    #endif
    for (int i = 0; i < vm->globalSlots.count; i++) {
        markValue(vm, vm->globalSlots.values[i]);
    }

    // String intern table weak roots - unmarked strings will be cleaned up in tableRemoveWhite
    for (int i = 0; i < vm->frame_count; i++) {
        markObject(vm, (Obj*)vm->frames[i].closure);
        if (vm->frames[i].caller_chunk != NULL) {
            markChunk(vm, vm->frames[i].caller_chunk);
        }
    }

    for (ObjUpvalue* upvalue = vm->open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
        markObject(vm, (Obj*)upvalue);
    }

    if (vm->chunk != NULL) {
        markChunk(vm, vm->chunk);
    }
    markChunk(vm, &vm->api_trampoline);
    #ifdef GC_DEBUG_FULL
    printf("Marking compiler roots (compiler=%p)\n", (void*)vm->compiler);
    fflush(stdout);
    #endif
    if (vm->compiler != NULL) {
        for (struct Compiler* compiler = vm->compiler; compiler != NULL; compiler = compiler->enclosing) {
            #ifdef GC_DEBUG_FULL
            printf("  Compiler chain: %p (enclosing=%p)\n", (void*)compiler, (void*)compiler->enclosing);
            fflush(stdout);
            #endif
            #ifdef GC_DEBUG_FULL
            printf("  Marking function being compiled: %p\n", (void*)compiler->function);
            fflush(stdout);
            #endif
            if (compiler->function != NULL) {
                if (compiler->current_module_name != NULL) {
                    markObject(vm, (Obj*)compiler->current_module_name);
                }
                Obj* fn_obj = (Obj*)compiler->function;
                if (fn_obj->type < 0 || fn_obj->type > 20) {
                    printf("  ERROR: compiler->function has invalid type %d, skipping\n", fn_obj->type);
                    fflush(stdout);
                } else {
                    markObject(vm, fn_obj);
                }
            }

            #ifdef GC_DEBUG_FULL
            printf("  Marking %d struct schemas\n", compiler->struct_schema_count);
            fflush(stdout);
            #endif
            for (int i = 0; i < compiler->struct_schema_count; i++) {
                #ifdef GC_DEBUG_FULL
                printf("    struct_schema[%d]: schema=%p, field_count=%d, field_names=%p\n",
                       i, (void*)compiler->struct_schemas[i].schema,
                       compiler->struct_schemas[i].field_count,
                       (void*)compiler->struct_schemas[i].field_names);
                fflush(stdout);
                #endif
                if (compiler->struct_schemas[i].schema) {
                    markObject(vm, (Obj*)compiler->struct_schemas[i].schema);
                }
                if (compiler->struct_schemas[i].field_names) {
                    for (int j = 0; j < compiler->struct_schemas[i].field_count; j++) {
                        if (compiler->struct_schemas[i].field_names[j]) {
                            markObject(vm, (Obj*)compiler->struct_schemas[i].field_names[j]);
                        }
                    }
                }
            }

            for (int i = 0; i < compiler->enum_schema_count; i++) {
                if (compiler->enum_schemas[i].schema) {
                    markObject(vm, (Obj*)compiler->enum_schemas[i].schema);
                }
                if (compiler->enum_schemas[i].variant_names) {
                    for (int j = 0; j < compiler->enum_schemas[i].variant_count; j++) {
                        if (compiler->enum_schemas[i].variant_names[j]) {
                            markObject(vm, (Obj*)compiler->enum_schemas[i].variant_names[j]);
                        }
                    }
                }
            }

            #ifdef GC_DEBUG_FULL
            printf("  Marking %d local struct types\n", compiler->local_count);
            fflush(stdout);
            #endif
            for (int i = 0; i < compiler->local_count; i++) {
                if (compiler->locals[i].struct_type) {
                    #ifdef GC_DEBUG_FULL
                    printf("    local[%d].struct_type = %p\n", i, (void*)compiler->locals[i].struct_type);
                    fflush(stdout);
                    #endif
                    markObject(vm, (Obj*)compiler->locals[i].struct_type);
                }
            }

            #ifdef GC_DEBUG_FULL
            printf("  Marking %d upvalue struct types\n", compiler->upvalue_count);
            fflush(stdout);
            #endif
            for (int i = 0; i < compiler->upvalue_count; i++) {
                if (compiler->upvalues[i].struct_type) {
                    #ifdef GC_DEBUG_FULL
                    printf("    upvalue[%d].struct_type = %p\n", i, (void*)compiler->upvalues[i].struct_type);
                    fflush(stdout);
                    #endif
                    markObject(vm, (Obj*)compiler->upvalues[i].struct_type);
                }
            }

            #ifdef GC_DEBUG_FULL
            printf("  Marking %d global types\n", compiler->global_type_count);
            fflush(stdout);
            #endif
            for (int i = 0; i < compiler->global_type_count; i++) {
                if (compiler->global_types[i].name) {
                    markObject(vm, (Obj*)compiler->global_types[i].name);
                }
                if (compiler->global_types[i].schema) {
                    #ifdef GC_DEBUG_FULL
                    printf("    global_type[%d].schema = %p\n", i, (void*)compiler->global_types[i].schema);
                    fflush(stdout);
                    #endif
                    markObject(vm, (Obj*)compiler->global_types[i].schema);
                }
            }
        }
    }

    for (int i = 0; i < vm->prompt_count; i++) {
        if (vm->prompt_stack[i].tag != NULL) {
            markObject(vm, (Obj*)vm->prompt_stack[i].tag);
        }
    }
}

static void traceReferences(VM* vm) {
    #ifdef GC_DEBUG_FULL
    printf("=== Starting traceReferences: gray_count=%d, gray_capacity=%d, gray_stack=%p ===\n",
           vm->gray_count, vm->gray_capacity, (void*)vm->gray_stack);
    fflush(stdout);
    #endif

    while (vm->gray_count > 0) {
        #ifdef GC_DEBUG_FULL
        printf("Loop iteration: gray_count=%d\n", vm->gray_count);
        fflush(stdout);
        #endif

        Obj* object = vm->gray_stack[--vm->gray_count];

        #ifdef GC_DEBUG_FULL
        printf("Processing gray object %p (remaining=%d)\n", (void*)object, vm->gray_count);
        fflush(stdout);
        #endif
        blackenObject(vm, object);
    }

    #ifdef GC_DEBUG_FULL
    printf("=== Finished traceReferences ===\n");
    fflush(stdout);
    #endif
}

static void blackenObject(VM* vm, Obj* object) {
    #ifdef GC_DEBUG_FULL
    printf("%p blacken [type=%d] ", (void*)object, object->type);
    fflush(stdout);
    printValue(vm, OBJ_VAL(object));
    printf("\n");
    fflush(stdout);
    #endif

    switch (object->type) {
        case OBJ_STRING:
        case OBJ_INT64:
            break;

        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening function: name=%p\n", (void*)function->name);
            fflush(stdout);
            #endif
            if (function->name != NULL) {
                markObject(vm, (Obj*)function->name);
            }
            if (function->module_name != NULL) {
                markObject(vm, (Obj*)function->module_name);
            }
            markChunk(vm, function->chunk);
            break;
        }

        case OBJ_NATIVE_FUNCTION: {
            ObjNativeFunction* native = (ObjNativeFunction*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening native function: name=%p\n", (void*)native->name);
            fflush(stdout);
            #endif
            if (native->name != NULL) {
                markObject(vm, (Obj*)native->name);
            }
            break;
        }

        case OBJ_NATIVE_CONTEXT:
            break;

        case OBJ_NATIVE_CLOSURE: {
            ObjNativeClosure* native_closure = (ObjNativeClosure*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening native closure: name=%p, context=%p\n",
                   (void*)native_closure->name, (void*)AS_OBJ(native_closure->context));
            fflush(stdout);
            #endif
            if (native_closure->name != NULL) {
                markObject(vm, (Obj*)native_closure->name);
            }
            markValue(vm, native_closure->context);
            break;
        }

        case OBJ_NATIVE_REFERENCE: {
            ObjNativeReference* native_ref = (ObjNativeReference*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening native reference: context=%p\n",
                   (void*)AS_OBJ(native_ref->context));
            fflush(stdout);
            #endif
            markValue(vm, native_ref->context);
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            markObject(vm, (Obj*)closure->function);
            if (closure->upvalues != NULL) {
                for (int i = 0; i < closure->upvalue_count; i++) {
                    markObject(vm, (Obj*)closure->upvalues[i]);
                }
            }
            break;
        }

        case OBJ_UPVALUE:
            markValue(vm, ((ObjUpvalue*)object)->closed);
            break;

        case OBJ_LIST: {
            ObjList* list = (ObjList*)object;
            for (int i = 0; i < list->items.count; i++) {
                markValue(vm, list->items.values[i]);
            }
            break;
        }

        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)object;
            if (map->table) {
                markTable(vm, map->table);
            }
            break;
        }

        case OBJ_REFERENCE: {
            ObjReference* ref = (ObjReference*)object;

            switch (ref->ref_type) {
                case REF_LOCAL:
                    if (ref->as.local.location != NULL) {
                        markValue(vm, *ref->as.local.location);
                    }
                    break;

                case REF_GLOBAL:
                    markObject(vm, (Obj*)ref->as.global.global_name);
                    break;

                case REF_INDEX:
                    markValue(vm, ref->as.index.container);
                    markValue(vm, ref->as.index.index);
                    break;

                case REF_PROPERTY:
                    markValue(vm, ref->as.property.container);
                    markValue(vm, ref->as.property.key);
                    break;

                case REF_UPVALUE:
                    markObject(vm, (Obj*)ref->as.upvalue.upvalue);
                    break;
            }
            break;
        }

        case OBJ_DISPATCHER: {
            ObjDispatcher* dispatcher = (ObjDispatcher*)object;
            for (int i = 0; i < dispatcher->count; i++) {
                markObject(vm, (Obj*)dispatcher->overloads[i]);
            }
            break;
        }

        case OBJ_STRUCT_SCHEMA: {
            ObjStructSchema* schema = (ObjStructSchema*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening struct schema: name=%p, field_count=%d, field_names=%p\n",
                   (void*)schema->name, schema->field_count, (void*)schema->field_names);
            fflush(stdout);
            #endif
            if (schema->name) {
                markObject(vm, (Obj*)schema->name);
            }
            if (schema->field_names && schema->field_count > 0) {
                for (int i = 0; i < schema->field_count; i++) {
                    if (schema->field_names[i]) {
                        #ifdef GC_DEBUG_FULL
                        printf("  Marking field name [%d]: %p\n", i, (void*)schema->field_names[i]);
                        fflush(stdout);
                        #endif
                        markObject(vm, (Obj*)schema->field_names[i]);
                    }
                }
            }
            if (schema->field_to_index) {
                #ifdef GC_DEBUG_FULL
                printf("  Marking field_to_index table: %p\n", (void*)schema->field_to_index);
                fflush(stdout);
                #endif
                markTable(vm, schema->field_to_index);
            }
            break;
        }

        case OBJ_STRUCT_INSTANCE: {
            ObjStructInstance* instance = (ObjStructInstance*)object;
            #ifdef GC_DEBUG_FULL
            printf("  Blackening struct instance: schema=%p, field_count=%d, fields=%p\n",
                   (void*)instance->schema, instance->field_count, (void*)instance->fields);
            fflush(stdout);
            #endif
            markObject(vm, (Obj*)instance->schema);
            if (instance->fields && instance->field_count > 0) {
                for (int i = 0; i < instance->field_count; i++) {
                    #ifdef GC_DEBUG_FULL
                    printf("  Marking field [%d]\n", i);
                    fflush(stdout);
                    #endif
                    markValue(vm, instance->fields[i]);
                }
            }
            break;
        }

        case OBJ_ENUM_SCHEMA: {
            ObjEnumSchema* schema = (ObjEnumSchema*)object;
            if (schema->name) {
                markObject(vm, (Obj*)schema->name);
            }
            if (schema->variant_names && schema->variant_count > 0) {
                for (int i = 0; i < schema->variant_count; i++) {
                    if (schema->variant_names[i]) {
                        markObject(vm, (Obj*)schema->variant_names[i]);
                    }
                }
            }
            break;
        }

        case OBJ_PROMPT_TAG: {
            ObjPromptTag* tag = (ObjPromptTag*)object;
            if (tag->name != NULL) {
                markObject(vm, (Obj*)tag->name);
            }
            break;
        }

        case OBJ_CONTINUATION: {
            ObjContinuation* cont = (ObjContinuation*)object;

            if (cont->prompt_tag != NULL) {
                markObject(vm, (Obj*)cont->prompt_tag);
            }

            for (int i = 0; i < cont->frame_count; i++) {
                if (cont->frames[i].closure != NULL) {
                    markObject(vm, (Obj*)cont->frames[i].closure);
                }
                if (cont->frames[i].caller_chunk != NULL) {
                    markChunk(vm, cont->frames[i].caller_chunk);
                }
            }

            for (int i = 0; i < cont->stack_size; i++) {
                markValue(vm, cont->stack[i]);
            }
            break;
        }
    }
}

static void sweep(VM* vm) {
    if (vm->gc_enabled) {
        fprintf(stderr, "FATAL: GC is enabled during sweep! This will cause corruption.\n");
        exit(1);
    }

    Obj** object = &vm->objects;
    while (*object != NULL) {
        if (!(*object)->is_marked) {
            Obj* unreached = *object;
            *object = unreached->next;

            #ifdef GC_DEBUG_FULL
            printf("%p free type %d (next=%p)", (void*)unreached, unreached->type, (void*)unreached->next);
            if (unreached->type == OBJ_FUNCTION) {
                ObjFunction* fn = (ObjFunction*)unreached;
                if (fn->name) {
                    printf(" [function: %.*s]", fn->name->length, fn->name->chars);
                }
            }
            printf("\n");
            fflush(stdout);
            #endif

            freeObject(vm, unreached);
        } else {
            (*object)->is_marked = false;
            object = &(*object)->next;
        }
    }
}

void freeObject(VM* vm, Obj* object) {
    #ifdef GC_DEBUG_FULL
    printf("%p free type %d\n", (void*)object, object->type);
    fflush(stdout);
    #endif

    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
            FREE_ARRAY(vm, char, string->chars, string->length + 1);
            FREE(vm, ObjString, object);
            break;
        }

        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            if (function->chunk) {
                if (function->chunk->code && function->chunk->capacity > 0) {
                    FREE_ARRAY(vm, uint32_t, function->chunk->code, function->chunk->capacity);
                }
                if (function->chunk->lines && function->chunk->capacity > 0) {
                    FREE_ARRAY(vm, int, function->chunk->lines, function->chunk->capacity);
                }
                if (function->chunk->constants.values && function->chunk->constants.capacity > 0) {
                    FREE_ARRAY(vm, Value, function->chunk->constants.values, function->chunk->constants.capacity);
                }
                FREE(vm, Chunk, function->chunk);
            }
            if (function->param_qualifiers && function->arity > 0) {
                FREE_ARRAY(vm, uint8_t, function->param_qualifiers, function->arity);
            }
            FREE(vm, ObjFunction, object);
            break;
        }

        case OBJ_NATIVE_FUNCTION: {
            ObjNativeFunction* native = (ObjNativeFunction*)object;
            if (native->param_qualifiers && native->arity > 0) {
                FREE_ARRAY(vm, uint8_t, native->param_qualifiers, native->arity);
            }
            FREE(vm, ObjNativeFunction, object);
            break;
        }

        case OBJ_NATIVE_CONTEXT: {
            ObjNativeContext* context = (ObjNativeContext*)object;
            if (context->finalizer) {
                context->finalizer(vm, context->native_data);
            }
            FREE(vm, ObjNativeContext, object);
            break;
        }

        case OBJ_NATIVE_CLOSURE: {
            ObjNativeClosure* closure = (ObjNativeClosure*)object;
            if (closure->param_qualifiers && closure->arity > 0) {
                FREE_ARRAY(vm, uint8_t, closure->param_qualifiers, closure->arity);
            }
            FREE(vm, ObjNativeClosure, object);
            break;
        }

        case OBJ_NATIVE_REFERENCE: {
            FREE(vm, ObjNativeReference, object);
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure* closure = (ObjClosure*)object;
            if (closure->upvalues != NULL && closure->upvalue_count > 0) {
                FREE_ARRAY(vm, ObjUpvalue*, closure->upvalues, closure->upvalue_count);
            }
            FREE(vm, ObjClosure, object);
            break;
        }

        case OBJ_UPVALUE:
            FREE(vm, ObjUpvalue, object);
            break;

        case OBJ_LIST: {
            ObjList* list = (ObjList*)object;
            if (list->items.values) {
                freeValueArray(vm, &list->items);
            }
            FREE(vm, ObjList, object);
            break;
        }

        case OBJ_MAP: {
            ObjMap* map = (ObjMap*)object;
            if (map->table) {
                freeTable(vm, map->table);
                FREE(vm, Table, map->table);
            }
            FREE(vm, ObjMap, object);
            break;
        }

        case OBJ_REFERENCE:
            FREE(vm, ObjReference, object);
            break;

        case OBJ_DISPATCHER: {
            FREE(vm, ObjDispatcher, object);
            break;
        }

        case OBJ_STRUCT_SCHEMA: {
            ObjStructSchema* schema = (ObjStructSchema*)object;
            if (schema->field_names) {
                FREE_ARRAY(vm, ObjString*, schema->field_names, schema->field_count);
            }
            if (schema->field_to_index) {
                freeTable(vm, schema->field_to_index);
                FREE(vm, Table, schema->field_to_index);
            }
            FREE(vm, ObjStructSchema, object);
            break;
        }

        case OBJ_STRUCT_INSTANCE: {
            ObjStructInstance* instance = (ObjStructInstance*)object;
            if (instance->fields && instance->field_count > 0) {
                FREE_ARRAY(vm, Value, instance->fields, instance->field_count);
            }
            FREE(vm, ObjStructInstance, object);
            break;
        }

        case OBJ_ENUM_SCHEMA: {
            ObjEnumSchema* schema = (ObjEnumSchema*)object;
            if (schema->variant_names && schema->variant_count > 0) {
                FREE_ARRAY(vm, ObjString*, schema->variant_names, schema->variant_count);
            }
            FREE(vm, ObjEnumSchema, object);
            break;
        }

        case OBJ_INT64:
            FREE(vm, ObjInt64, object);
            break;

        case OBJ_PROMPT_TAG:
            FREE(vm, ObjPromptTag, object);
            break;

        case OBJ_CONTINUATION: {
            ObjContinuation* cont = (ObjContinuation*)object;

            if (cont->frames != NULL && cont->frame_count > 0) {
                FREE_ARRAY(vm, CallFrame, cont->frames, cont->frame_count);
            }

            if (cont->stack != NULL && cont->stack_size > 0) {
                FREE_ARRAY(vm, Value, cont->stack, cont->stack_size);
            }

            FREE(vm, ObjContinuation, object);
            break;
        }
    }
}

void tableRemoveWhite(Table* table) {
    for (int i = 0; i < table->capacity; i++) {
        Entry* entry = &table->entries[i];
        if (entry->key != NULL && !entry->key->obj.is_marked) {
            #ifdef GC_DEBUG_FULL
            printf("Removing unmarked string from intern table: %p \"%.*s\"\n",
                   (void*)entry->key, entry->key->length, entry->key->chars);
            fflush(stdout);
            #endif
            tableDelete(table, entry->key);
        }
    }
}

void collectGarbage(VM* vm) {
    #if defined(GC_DEBUG) || defined(GC_DEBUG_FULL)
    size_t before = vm->bytes_allocated;
    #endif

    #ifdef GC_DEBUG_FULL
    printf("-- gc begin\n");
    printf("Initial state: gray_count=%d, gray_capacity=%d, gray_stack=%p\n",
           vm->gray_count, vm->gray_capacity, (void*)vm->gray_stack);
    fflush(stdout);
    #endif

    bool was_enabled = vm->gc_enabled;
    vm->gc_enabled = false;

    #ifdef GC_DEBUG_FULL
    printf("=== Phase 1: Marking roots ===\n");
    fflush(stdout);
    #endif
    markRoots(vm);

    traceReferences(vm);

    #ifdef GC_DEBUG_FULL
    printf("=== Phase 3: Removing unmarked strings from intern table ===\n");
    fflush(stdout);
    #endif
    tableRemoveWhite(&vm->strings);

    #ifdef GC_DEBUG_FULL
    printf("=== Phase 4: Sweeping unmarked objects ===\n");
    fflush(stdout);
    #endif
    sweep(vm);

    vm->next_gc = vm->bytes_allocated * GC_HEAP_GROW_FACTOR;

    vm->gc_enabled = was_enabled;

    #if defined(GC_DEBUG) || defined(GC_DEBUG_FULL)
    printf("   collected %zu bytes (from %zu to %zu) next at %zu\n",
           before - vm->bytes_allocated, before, vm->bytes_allocated, vm->next_gc);
    fflush(stdout);
    #endif

    #ifdef GC_DEBUG_FULL
    printf("=== GC completed, returning to program ===\n");
    fflush(stdout);
    #endif
}
