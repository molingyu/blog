#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "MEM.h"
#include "DBG.h"
#include "diksamc.h"

extern OpcodeInfo dvm_opcode_info[];

#define OPCODE_ALLOC_SIZE (256)
#define LABEL_TABLE_ALLOC_SIZE (256)

typedef struct {
    int label_address;
} LabelTable;

typedef struct {
    int         size;
    int         alloc_size;
    DVM_Byte    *code;
    int         label_table_size;
    int         label_table_alloc_size;
    LabelTable  *label_table;
    int         line_number_size;
    DVM_LineNumber      *line_number;
    int         try_size;
    DVM_Try     *try;
} OpcodeBuf;

static DVM_Executable *
alloc_executable(PackageName *package_name)
{
    DVM_Executable      *exe;

    exe = MEM_malloc(sizeof(DVM_Executable));
    exe->package_name = dkc_package_name_to_string(package_name);
    exe->is_required = DVM_FALSE;
    exe->constant_pool_count = 0;
    exe->constant_pool = NULL;
    exe->global_variable_count = 0;
    exe->global_variable = NULL;
    exe->function_count = 0;
    exe->function = NULL;
    exe->constant_count = 0;
    exe->constant_definition = NULL;
    exe->type_specifier_count = 0;
    exe->type_specifier = NULL;
    exe->top_level.code_size = 0;
    exe->top_level.code = NULL;

    return exe;
}

static int
add_constant_pool(DVM_Executable *exe, DVM_ConstantPool *cp)
{
    int ret;

    exe->constant_pool
        = MEM_realloc(exe->constant_pool,
                      sizeof(DVM_ConstantPool)
                      * (exe->constant_pool_count + 1));
    exe->constant_pool[exe->constant_pool_count] = *cp;

    ret = exe->constant_pool_count;
    exe->constant_pool_count++;

    return ret;
}

static int
count_parameter(ParameterList *src)
{
    ParameterList *param;
    int param_count = 0;

    for (param = src; param; param = param->next) {
        param_count++;
    }

    return param_count;
}

static DVM_LocalVariable *
copy_parameter_list(ParameterList *src, int *param_count_p)
{
    ParameterList *param;
    DVM_LocalVariable *dest;
    int param_count;
    int i;

    param_count = count_parameter(src);
    *param_count_p = param_count;

    dest = MEM_malloc(sizeof(DVM_LocalVariable) * param_count);
    
    for (param = src, i = 0; param; param = param->next, i++) {
        dest[i].name = MEM_strdup(param->name);
        dest[i].type = dkc_copy_type_specifier(param->type);
    }

    return dest;
}

static void
copy_type_specifier_no_alloc(TypeSpecifier *src, DVM_TypeSpecifier *dest)
{
    int derive_count = 0;
    TypeDerive *derive;
    int param_count;
    int i;

    dest->basic_type = src->basic_type;
    if (src->basic_type == DVM_CLASS_TYPE) {
        dest->u.class_t.index = src->u.class_ref.class_index;
    } else {
        dest->u.class_t.index = -1;
    }

    for (derive = src->derive; derive; derive = derive->next) {
        derive_count++;
    }
    dest->derive_count = derive_count;
    dest->derive = MEM_malloc(sizeof(DVM_TypeDerive) * derive_count);
    for (i = 0, derive = src->derive; derive;
         derive = derive->next, i++) {
        switch (derive->tag) {
        case FUNCTION_DERIVE:
            dest->derive[i].tag = DVM_FUNCTION_DERIVE;
            dest->derive[i].u.function_d.parameter
                = copy_parameter_list(derive->u.function_d.parameter_list,
                                      &param_count);
            dest->derive[i].u.function_d.parameter_count = param_count;
            break;
        case ARRAY_DERIVE:
            dest->derive[i].tag = DVM_ARRAY_DERIVE;
            break;
        default:
            DBG_assert(0, ("derive->tag..%d\n", derive->tag));
        }
    }
}

DVM_TypeSpecifier *
dkc_copy_type_specifier(TypeSpecifier *src)
{
    DVM_TypeSpecifier *dest;

    dest = MEM_malloc(sizeof(DVM_TypeSpecifier));

    copy_type_specifier_no_alloc(src, dest);

    return dest;
}

static void
add_global_variable(DKC_Compiler *compiler, DVM_Executable *exe)
{
    DeclarationList *dl;
    int i;
    int var_count = 0;

    for (dl = compiler->declaration_list; dl; dl = dl->next) {
        var_count++;
    }
    exe->global_variable_count = var_count;
    exe->global_variable = MEM_malloc(sizeof(DVM_Variable) * var_count);

    for (dl = compiler->declaration_list, i = 0; dl; dl = dl->next, i++) {
        exe->global_variable[i].name = MEM_strdup(dl->declaration->name);
        exe->global_variable[i].type
            = dkc_copy_type_specifier(dl->declaration->type);
    }
}

static void
add_method(DVM_Executable *exe,
           MemberDeclaration *member, DVM_Method *dest,
           DVM_Boolean is_implemented)
{
    FunctionDefinition *fd;

    dest->access_modifier = member->access_modifier;
    dest->is_abstract = member->u.method.is_abstract;
    dest->is_virtual = member->u.method.is_virtual;
    dest->is_override = member->u.method.is_override;
    dest->name = MEM_strdup(member->u.method.function_definition->name);

    fd = member->u.method.function_definition;
}

static void
add_field(MemberDeclaration *member, DVM_Field *dest)
{
    dest->access_modifier = member->access_modifier;
    dest->name = MEM_strdup(member->u.field.name);
    dest->type = dkc_copy_type_specifier(member->u.field.type);
}

static void
set_class_identifier(ClassDefinition *cd, DVM_ClassIdentifier *ci)
{
    ci->name = MEM_strdup(cd->name);
    ci->package_name = dkc_package_name_to_string(cd->package_name);
}

static DVM_Class *
search_class(DKC_Compiler *compiler, ClassDefinition *src)
{
    int i;
    char *src_package_name;

    src_package_name = dkc_package_name_to_string(src->package_name);
    for (i = 0; i < compiler->dvm_class_count; i++) {
        if (dvm_compare_package_name(src_package_name,
                                     compiler->dvm_class[i].package_name)
            && !strcmp(src->name, compiler->dvm_class[i].name)) {
            MEM_free(src_package_name);
            return &compiler->dvm_class[i];
        }
    }
    DBG_assert(0, ("function %s::%s not found.", src_package_name, src->name));

    return NULL; /* make compiler happy. */
}

static void
add_class(DVM_Executable *exe, ClassDefinition *cd, DVM_Class *dest)
{
    int interface_count = 0;
    int method_count = 0;
    int field_count = 0;
    MemberDeclaration *pos;
    ExtendsList *if_pos;

    dest->is_abstract = cd->is_abstract;
    dest->access_modifier = cd->access_modifier;
    dest->class_or_interface = cd->class_or_interface;

    if (cd->super_class) {
        dest->super_class = MEM_malloc(sizeof(DVM_ClassIdentifier));
        set_class_identifier(cd->super_class, dest->super_class);
    } else {
        dest->super_class = NULL;
    }
    for (if_pos = cd->interface_list; if_pos; if_pos = if_pos->next) {
        interface_count++;
    }
    dest->interface_count = interface_count;
    dest->interface_ = MEM_malloc(sizeof(DVM_ClassIdentifier)
                                  * interface_count);
    interface_count = 0;
    for (if_pos = cd->interface_list; if_pos; if_pos = if_pos->next) {
        set_class_identifier(if_pos->class_definition,
                             &dest->interface_[interface_count]);
        interface_count++;
    }

    for (pos = cd->member; pos; pos = pos->next) {
        if (pos->kind == METHOD_MEMBER) {
            method_count++;
        } else {
            DBG_assert(pos->kind == FIELD_MEMBER,
                       ("pos->kind..%d", pos->kind));
            field_count++;
        }
    }
    dest->field_count = field_count;
    dest->field = MEM_malloc(sizeof(DVM_Field) * field_count);
    dest->method_count = method_count;
    dest->method = MEM_malloc(sizeof(DVM_Method) * method_count);

    method_count = field_count = 0;
    for (pos = cd->member; pos; pos = pos->next) {
        if (pos->kind == METHOD_MEMBER) {
            add_method(exe, pos, &dest->method[method_count],
                       dest->is_implemented);
            method_count++;
        } else {
            DBG_assert(pos->kind == FIELD_MEMBER,
                       ("pos->kind..%d", pos->kind));
            add_field(pos, &dest->field[field_count]);
            field_count++;
        }
    }
}

static void
init_opcode_buf(OpcodeBuf *ob)
{
    ob->size = 0;
    ob->alloc_size = 0;
    ob->code = NULL;
    ob->label_table_size = 0;
    ob->label_table_alloc_size = 0;
    ob->label_table = NULL;
    ob->line_number_size = 0;
    ob->line_number = NULL;
    ob->try_size = 0;
    ob->try = NULL;
}

static void
fix_labels(OpcodeBuf *ob)
{
    int i;
    int j;
    OpcodeInfo *info;
    int label;
    int address;

    for (i = 0; i < ob->size; i++) {
        if (ob->code[i] == DVM_JUMP
            || ob->code[i] == DVM_JUMP_IF_TRUE
            || ob->code[i] == DVM_JUMP_IF_FALSE
            || ob->code[i] == DVM_GO_FINALLY) {
            label = (ob->code[i+1] << 8) + (ob->code[i+2]);
            address = ob->label_table[label].label_address;
            ob->code[i+1] = (DVM_Byte)(address >> 8);
            ob->code[i+2] = (DVM_Byte)(address &0xff);
        }
        info = &dvm_opcode_info[ob->code[i]];
        for (j = 0; info->parameter[j] != '\0'; j++) {
            switch (info->parameter[j]) {
            case 'b':
                i++;
                break;
            case 's': /* FALLTHRU */
            case 'p':
                i += 2;
                break;
            default:
                DBG_assert(0, ("param..%s, j..%d", info->parameter, j));
            }
        }
    }
}

static DVM_Byte *
fix_opcode_buf(OpcodeBuf *ob)
{
    DVM_Byte *ret;

    fix_labels(ob);
    ret = MEM_realloc(ob->code, ob->size);
    MEM_free(ob->label_table);

    return ret;
}

static int
calc_need_stack_size(DVM_Byte *code, int code_size)
{
    int i, j;
    int stack_size = 0;
    OpcodeInfo  *info;

    for (i = 0; i < code_size; i++) {
        info = &dvm_opcode_info[code[i]];
        if (info->stack_increment > 0) {
            stack_size += info->stack_increment;
        }
        for (j = 0; info->parameter[j] != '\0'; j++) {
            switch (info->parameter[j]) {
            case 'b':
                i++;
                break;
            case 's': /* FALLTHRU */
            case 'p':
                i += 2;
                break;
            default:
                DBG_assert(0, ("param..%s, j..%d", info->parameter, j));
            }
        }
    }

    return stack_size;
}

static void
add_line_number(OpcodeBuf *ob, int line_number, int start_pc)
{
    if (ob->line_number == NULL
        || (ob->line_number[ob->line_number_size-1].line_number
            != line_number)) {
        ob->line_number = MEM_realloc(ob->line_number,
                                      sizeof(DVM_LineNumber)
                                      * (ob->line_number_size + 1));

        ob->line_number[ob->line_number_size].line_number = line_number;
        ob->line_number[ob->line_number_size].start_pc = start_pc;
        ob->line_number[ob->line_number_size].pc_count
            = ob->size - start_pc;
        ob->line_number_size++;

    } else {
        ob->line_number[ob->line_number_size-1].pc_count
            += ob->size - start_pc;
    }
}

static void
generate_code(OpcodeBuf *ob, int line_number, DVM_Opcode code,  ...)
{
    va_list     ap;
    int         i;
    char        *param;
    int         param_count;
    int         start_pc;

    va_start(ap, code);

    param = dvm_opcode_info[(int)code].parameter;
    param_count = strlen(param);
    if (ob->alloc_size < ob->size + 1 + (param_count * 2)) {
        ob->code = MEM_realloc(ob->code, ob->alloc_size + OPCODE_ALLOC_SIZE);
        ob->alloc_size += OPCODE_ALLOC_SIZE;
    }

    start_pc = ob->size;
    ob->code[ob->size] = code;
    ob->size++;
    for (i = 0; param[i] != '\0'; i++) {
        unsigned int value = va_arg(ap, int);
        switch (param[i]) {
        case 'b': /* byte */
            ob->code[ob->size] = (DVM_Byte)value;
            ob->size++;
            break;
        case 's': /* short(2byte int) */
            ob->code[ob->size] = (DVM_Byte)(value >> 8);
            ob->code[ob->size+1] = (DVM_Byte)(value & 0xff);
            ob->size += 2;
            break;
        case 'p': /* constant pool index */
            ob->code[ob->size] = (DVM_Byte)(value >> 8);
            ob->code[ob->size+1] = (DVM_Byte)(value & 0xff);
            ob->size += 2;
            break;
        default:
            DBG_assert(0, ("param..%s, i..%d", param, i));
        }
    }
    add_line_number(ob, line_number, start_pc);

    va_end(ap);
}

static int
get_opcode_type_offset(TypeSpecifier *type)
{
    if (type->derive != NULL) {
        DBG_assert(type->derive->tag = ARRAY_DERIVE,
                   ("type->derive->tag..%d", type->derive->tag));
        return 2;
    }

    switch (type->basic_type) {
    case DVM_VOID_TYPE:
        DBG_assert(0, ("basic_type is void"));
        break;
    case DVM_BOOLEAN_TYPE: /* FALLTHRU */
    case DVM_INT_TYPE: /* FALLTHRU */
    case DVM_ENUM_TYPE:
        return 0;
        break;
    case DVM_DOUBLE_TYPE:
        return 1;
        break;
    case DVM_STRING_TYPE: /* FALLTHRU */
    case DVM_NATIVE_POINTER_TYPE: /* FALLTHRU */
    case DVM_CLASS_TYPE: /* FALLTHRU */
    case DVM_DELEGATE_TYPE: /* FALLTHRU */
        return 2;
        break;
    case DVM_NULL_TYPE: /* FALLTHRU */
    case DVM_BASE_TYPE: /* FALLTHRU */
    case DVM_UNSPECIFIED_IDENTIFIER_TYPE: /* FALLTHRU */
    default:
        DBG_assert(0, ("basic_type..%d", type->basic_type));
    }

    return 0;
}

static void generate_expression(DVM_Executable *exe, Block *current_block,
                                Expression *expr, OpcodeBuf *ob);

static void
copy_opcode_buf(DVM_CodeBlock *dest, OpcodeBuf *ob)
{
    dest->code_size = ob->size;
    dest->code = fix_opcode_buf(ob);
    dest->line_number_size = ob->line_number_size;
    dest->line_number = ob->line_number;
    dest->try_size = ob->try_size;
    dest->try = ob->try;
    dest->need_stack_size = calc_need_stack_size(dest->code,
                                                 dest->code_size);
}

static void
generate_field_initializer(DVM_Executable *exe,
                           ClassDefinition *cd, DVM_Class *dvm_class)
{
    OpcodeBuf ob;
    ClassDefinition *cd_pos;
    MemberDeclaration *member_pos;

    init_opcode_buf(&ob);

    for (cd_pos = cd; cd_pos; cd_pos = cd_pos->super_class) {
        for (member_pos = cd_pos->member; member_pos;
             member_pos = member_pos->next) {
            if (member_pos->kind != FIELD_MEMBER)
                continue;

            if (member_pos->u.field.initializer) {
                generate_expression(exe, NULL, member_pos->u.field.initializer,
                                    &ob);
                generate_code(&ob,
                              member_pos->u.field.initializer->line_number,
                              DVM_DUPLICATE_OFFSET, 1);
                generate_code(&ob,
                              member_pos->u.field.initializer->line_number,
                              DVM_POP_FIELD_INT
                              + get_opcode_type_offset(member_pos
                                                       ->u.field.type),
                              member_pos->u.field.field_index);
            }
        }
    }
    copy_opcode_buf(&dvm_class->field_initializer, &ob);
}

static void
add_classes(DKC_Compiler *compiler, DVM_Executable *exe)
{
    ClassDefinition *cd_pos;
    int i;
    ClassDefinition *cd;
    DVM_Class *dvm_class;

    for (cd_pos = compiler->class_definition_list; cd_pos;
         cd_pos = cd_pos->next) {
        dvm_class = search_class(compiler, cd_pos);
        dvm_class->is_implemented = DVM_TRUE;
        generate_field_initializer(exe, cd_pos, dvm_class);
    }

    for (i = 0; i < compiler->dvm_class_count; i++) {
        cd = dkc_search_class(compiler->dvm_class[i].name);
        add_class(exe, cd, &compiler->dvm_class[i]);
    }
}

static int
add_type_specifier(TypeSpecifier *src, DVM_Executable *exe)
{
    int ret;

    exe->type_specifier
        = MEM_realloc(exe->type_specifier,
                      sizeof(DVM_TypeSpecifier)
                      * (exe->type_specifier_count + 1));
    copy_type_specifier_no_alloc(src,
                                 &exe->type_specifier
                                 [exe->type_specifier_count]);

    ret = exe->type_specifier_count;
    exe->type_specifier_count++;

    return ret;
}

static void
generate_boolean_expression(DVM_Executable *cf, Expression *expr,
                            OpcodeBuf *ob)
{
    if (expr->u.boolean_value) {
        generate_code(ob, expr->line_number, DVM_PUSH_INT_1BYTE, 1);
    } else {
        generate_code(ob, expr->line_number, DVM_PUSH_INT_1BYTE, 0);
    }
}

static void
generate_int_expression(DVM_Executable *cf, int line_number, int value,
                        OpcodeBuf *ob)
{
    DVM_ConstantPool cp;
    int cp_idx;

    if (value >= 0 && value < 256) {
        generate_code(ob, line_number, DVM_PUSH_INT_1BYTE, value);
    } else if (value >= 0 && value < 65536) {
        generate_code(ob, line_number, DVM_PUSH_INT_2BYTE, value);
    } else {
        cp.tag = DVM_CONSTANT_INT;
        cp.u.c_int = value;
        cp_idx = add_constant_pool(cf, &cp);

        generate_code(ob, line_number, DVM_PUSH_INT, cp_idx);
    }
}

static void
generate_double_expression(DVM_Executable *cf, Expression *expr,
                           OpcodeBuf *ob)
{
    DVM_ConstantPool cp;
    int cp_idx;

    if (expr->u.double_value == 0.0) {
        generate_code(ob, expr->line_number, DVM_PUSH_DOUBLE_0);
    } else if (expr->u.double_value == 1.0) {
        generate_code(ob, expr->line_number, DVM_PUSH_DOUBLE_1);
    } else {
        cp.tag = DVM_CONSTANT_DOUBLE;
        cp.u.c_double = expr->u.double_value;
        cp_idx = add_constant_pool(cf, &cp);

        generate_code(ob, expr->line_number, DVM_PUSH_DOUBLE, cp_idx);
    }
}

static void
generate_string_expression(DVM_Executable *cf, Expression *expr,
                           OpcodeBuf *ob)
{
    DVM_ConstantPool cp;
    int cp_idx;

    cp.tag = DVM_CONSTANT_STRING;
    cp.u.c_string = expr->u.string_value;
    cp_idx = add_constant_pool(cf, &cp);
    generate_code(ob, expr->line_number, DVM_PUSH_STRING, cp_idx);
}

static void
generate_identifier(Declaration *decl, OpcodeBuf *ob, int line_number)
{
    if (decl->is_local) {
        generate_code(ob, line_number,
                      DVM_PUSH_STACK_INT
                      + get_opcode_type_offset(decl->type),
                      decl->variable_index);
    } else {
        generate_code(ob, line_number,
                      DVM_PUSH_STATIC_INT
                      + get_opcode_type_offset(decl->type),
                      decl->variable_index);
    }
}

static void
generate_identifier_expression(DVM_Executable *exe, Block *block,
                               Expression *expr, OpcodeBuf *ob)
{
    switch (expr->u.identifier.kind) {
    case VARIABLE_IDENTIFIER:
        generate_identifier(expr->u.identifier.u.declaration, ob,
                            expr->line_number);
        break;
    case FUNCTION_IDENTIFIER:
        generate_code(ob, expr->line_number,
                      DVM_PUSH_FUNCTION,
                      expr->u.identifier.u.function.function_index);
        break;
    case CONSTANT_IDENTIFIER:
        generate_code(ob, expr->line_number,
                      DVM_PUSH_CONSTANT_INT
                      + get_opcode_type_offset(expr->u.identifier.u.constant
                                               .constant_definition->type),
                      expr->u.identifier.u.constant.constant_index);
        break;
    default:
        DBG_panic(("bad default. kind..%d", expr->u.identifier.kind));
    }
}


static void
generate_pop_to_identifier(Declaration *decl, int line_number,
                           OpcodeBuf *ob)
{
    if (decl->is_local) {
        generate_code(ob, line_number,
                      DVM_POP_STACK_INT
                      + get_opcode_type_offset(decl->type),
                      decl->variable_index);
    } else {
        generate_code(ob, line_number,
                      DVM_POP_STATIC_INT
                      + get_opcode_type_offset(decl->type),
                      decl->variable_index);
    }
}

static void
generate_pop_to_member(DVM_Executable *exe, Block *block,
                       Expression *expr, OpcodeBuf *ob)
{
    MemberDeclaration *member;

    member = expr->u.member_expression.declaration;
    if (member->kind == METHOD_MEMBER) {
        dkc_compile_error(expr->line_number, ASSIGN_TO_METHOD_ERR,
                          STRING_MESSAGE_ARGUMENT, "member_name",
                          member->u.method.function_definition->name,
                          MESSAGE_ARGUMENT_END);
    }
    generate_expression(exe, block, expr->u.member_expression.expression, ob);
    generate_code(ob, expr->line_number,
                  DVM_POP_FIELD_INT
                  + get_opcode_type_offset(member->u.field.type),
                  member->u.field.field_index);
}

static void
generate_pop_to_lvalue(DVM_Executable *exe, Block *block,
                       Expression *expr, OpcodeBuf *ob)
{
    if (expr->kind == IDENTIFIER_EXPRESSION) {
        generate_pop_to_identifier(expr->u.identifier.u.declaration,
                                   expr->line_number,
                                   ob);
    } else if (expr->kind == INDEX_EXPRESSION) {
        generate_expression(exe, block, expr->u.index_expression.array, ob);
        generate_expression(exe, block, expr->u.index_expression.index, ob);
        generate_code(ob, expr->line_number,
                      DVM_POP_ARRAY_INT
                      + get_opcode_type_offset(expr->type));

    } else {
        DBG_assert(expr->kind == MEMBER_EXPRESSION,
                   ("expr->kind..%d", expr->kind));
        generate_pop_to_member(exe, block, expr, ob);
    }
}

static void
generate_assign_expression(DVM_Executable *exe, Block *block,
                           Expression *expr, OpcodeBuf *ob,
                           DVM_Boolean is_toplevel)
{
    if (expr->u.assign_expression.operator != NORMAL_ASSIGN) {
        generate_expression(exe, block, 
                            expr->u.assign_expression.left, ob);
    }
    generate_expression(exe, block, expr->u.assign_expression.operand, ob);

    switch (expr->u.assign_expression.operator) {
    case NORMAL_ASSIGN :
        break;
    case ADD_ASSIGN:
        generate_code(ob, expr->line_number,
                      DVM_ADD_INT
                      + get_opcode_type_offset(expr->type));
        break;
    case SUB_ASSIGN:
        generate_code(ob, expr->line_number,
                      DVM_SUB_INT
                      + get_opcode_type_offset(expr->type));
        break;
    case MUL_ASSIGN:
        generate_code(ob, expr->line_number,
                      DVM_MUL_INT
                      + get_opcode_type_offset(expr->type));
        break;
    case DIV_ASSIGN:
        generate_code(ob, expr->line_number,
                      DVM_DIV_INT
                      + get_opcode_type_offset(expr->type));
        break;
    case MOD_ASSIGN:
        generate_code(ob, expr->line_number,
                      DVM_MOD_INT
                      + get_opcode_type_offset(expr->type));
        break;
    default:
        DBG_assert(0, ("operator..%d\n", expr->u.assign_expression.operator));
    }

    if (!is_toplevel) {
        generate_code(ob, expr->line_number, DVM_DUPLICATE);
    }
    generate_pop_to_lvalue(exe, block,
                           expr->u.assign_expression.left, ob);
}

static int
get_binary_expression_offset(Expression *left, Expression *right,
                             DVM_Opcode code)
{
    int offset;

    if ((left->kind == NULL_EXPRESSION && right->kind != NULL_EXPRESSION)
        || (left->kind != NULL_EXPRESSION && right->kind == NULL_EXPRESSION)) {
        offset = 2; /* object type */

    } else if ((code == DVM_EQ_INT || code == DVM_NE_INT)
               && dkc_is_string(left->type)) {
        offset = 3; /* string type */

    } else {
        offset = get_opcode_type_offset(left->type);
    }

    return offset;
}

static void
generate_binary_expression(DVM_Executable *exe, Block *block,
                           Expression *expr, DVM_Opcode code,
                           OpcodeBuf *ob)
{
    int offset;
    Expression *left = expr->u.binary_expression.left;
    Expression *right = expr->u.binary_expression.right;

    generate_expression(exe, block, left, ob);
    generate_expression(exe, block, right, ob);

    offset = get_binary_expression_offset(left, right, code);

    generate_code(ob, expr->line_number,
                  code + offset);
}

static void
generate_bit_binary_expression(DVM_Executable *exe, Block *block,
                               Expression *expr, DVM_Opcode code,
                               OpcodeBuf *ob)
{
    Expression *left = expr->u.binary_expression.left;
    Expression *right = expr->u.binary_expression.right;

    generate_expression(exe, block, left, ob);
    generate_expression(exe, block, right, ob);

    generate_code(ob, expr->line_number, code);
}

static int
get_label(OpcodeBuf *ob)
{
    int ret;

    if (ob->label_table_alloc_size < ob->label_table_size + 1) {
        ob->label_table = MEM_realloc(ob->label_table,
                                      (ob->label_table_alloc_size
                                       + LABEL_TABLE_ALLOC_SIZE)
                                      * sizeof(LabelTable));
        ob->label_table_alloc_size += LABEL_TABLE_ALLOC_SIZE;
    }
    ret = ob->label_table_size;
    ob->label_table_size++;

    return ret;
}

static void
set_label(OpcodeBuf *ob, int label)
{
    ob->label_table[label].label_address = ob->size;
}

static void
generate_logical_and_expression(DVM_Executable *exe, Block *block,
                                Expression *expr,
                                OpcodeBuf *ob)
{
    int false_label;

    false_label = get_label(ob);
    generate_expression(exe, block, expr->u.binary_expression.left, ob);
    generate_code(ob, expr->line_number, DVM_DUPLICATE);
    generate_code(ob, expr->line_number, DVM_JUMP_IF_FALSE, false_label);
    generate_expression(exe, block, expr->u.binary_expression.right, ob);
    generate_code(ob, expr->line_number, DVM_LOGICAL_AND);
    set_label(ob, false_label);
}

static void
generate_logical_or_expression(DVM_Executable *exe, Block *block,
                               Expression *expr,
                               OpcodeBuf *ob)
{
    int true_label;

    true_label = get_label(ob);
    generate_expression(exe, block, expr->u.binary_expression.left, ob);
    generate_code(ob, expr->line_number, DVM_DUPLICATE);
    generate_code(ob, expr->line_number, DVM_JUMP_IF_TRUE, true_label);
    generate_expression(exe, block, expr->u.binary_expression.right, ob);
    generate_code(ob, expr->line_number, DVM_LOGICAL_OR);
    set_label(ob, true_label);
}

static void
generate_cast_expression(DVM_Executable *exe, Block *block,
                         Expression *expr, OpcodeBuf *ob)
{
    switch (expr->u.cast.type) {
    case INT_TO_DOUBLE_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_INT_TO_DOUBLE);
        break;
    case DOUBLE_TO_INT_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_DOUBLE_TO_INT);
        break;
    case BOOLEAN_TO_STRING_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_BOOLEAN_TO_STRING);
        break;
    case INT_TO_STRING_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_INT_TO_STRING);
        break;
    case DOUBLE_TO_STRING_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_DOUBLE_TO_STRING);
        break;
    case ENUM_TO_STRING_CAST:
        generate_expression(exe, block, expr->u.cast.operand, ob);
        generate_code(ob, expr->line_number, DVM_CAST_ENUM_TO_STRING,
                      expr->u.cast.operand->type->u.enum_ref.enum_index);
        break;
    case FUNCTION_TO_DELEGATE_CAST:
        if (expr->u.cast.operand->kind == IDENTIFIER_EXPRESSION) {
            generate_code(ob, expr->line_number, DVM_PUSH_DELEGATE,
                          expr->u.cast.operand->u.identifier.u.function
                          .function_index);
        } else {
            /* Method's delegate is generated in generate_member_expression().
             */
            DBG_assert(expr->u.cast.operand->kind == MEMBER_EXPRESSION,
                       ("kind..%d", expr->u.cast.operand->kind));
            generate_expression(exe, block, expr->u.cast.operand, ob);
        }
        break;
    default:
        DBG_assert(0, ("expr->u.cast.type..%d", expr->u.cast.type));
    }
}

static void
generate_up_cast_expression(DVM_Executable *exe, Block *block,
                            Expression *expr, OpcodeBuf *ob)
{
    generate_expression(exe, block, expr->u.up_cast.operand, ob);

    generate_code(ob, expr->line_number, DVM_UP_CAST,
                  expr->u.up_cast.interface_index);
}

static void
generate_array_literal_expression(DVM_Executable *exe, Block *block,
                                  Expression *expr, OpcodeBuf *ob)
{
    ExpressionList *pos;
    int count;

    DBG_assert(expr->type->derive
               && expr->type->derive->tag == ARRAY_DERIVE,
               ("array literal is not array."));

    count = 0;
    for (pos = expr->u.array_literal; pos; pos = pos->next) {
        generate_expression(exe, block, pos->expression, ob);
        count++;
    }
    DBG_assert(count > 0, ("empty array literal"));
    
    generate_code(ob, expr->line_number,
                  DVM_NEW_ARRAY_LITERAL_INT
                  + get_opcode_type_offset(expr->u.array_literal
                                           ->expression->type),
                  count);
}

static void
generate_index_expression(DVM_Executable *exe, Block *block,
                          Expression *expr, OpcodeBuf *ob)
{
    generate_expression(exe, block, expr->u.index_expression.array, ob);
    generate_expression(exe, block, expr->u.index_expression.index, ob);

    if (dkc_is_string(expr->u.index_expression.array->type)) {
        generate_code(ob, expr->line_number, DVM_PUSH_CHARACTER_IN_STRING);
    } else {
        generate_code(ob, expr->line_number, DVM_PUSH_ARRAY_INT
                      + get_opcode_type_offset(expr->type));
    }
}

static void
generate_inc_dec_expression(DVM_Executable *exe, Block *block,
                            Expression *expr, ExpressionKind kind,
                            OpcodeBuf *ob, DVM_Boolean is_toplevel)
{
    generate_expression(exe, block, expr->u.inc_dec.operand, ob);

    if (kind == INCREMENT_EXPRESSION) {
        generate_code(ob, expr->line_number, DVM_INCREMENT);
    } else {
        DBG_assert(kind == DECREMENT_EXPRESSION, ("kind..%d\n", kind));
        generate_code(ob, expr->line_number, DVM_DECREMENT);
    }
    if (!is_toplevel) {
        generate_code(ob, expr->line_number, DVM_DUPLICATE);
    }
    generate_pop_to_lvalue(exe, block,
                           expr->u.inc_dec.operand, ob);
}

static void
generate_instanceof_expression(DVM_Executable *exe, Block *block,
                               Expression *expr, OpcodeBuf *ob)
{
    generate_expression(exe, block, expr->u.instanceof.operand, ob);
    generate_code(ob, expr->line_number, DVM_INSTANCEOF,
                  expr->u.instanceof.type->u.class_ref.class_index);
}

static void
generate_down_cast_expression(DVM_Executable *exe, Block *block,
                              Expression *expr, OpcodeBuf *ob)
{
    generate_expression(exe, block, expr->u.down_cast.operand, ob);
    generate_code(ob, expr->line_number, DVM_DOWN_CAST,
                  expr->u.down_cast.type->u.class_ref.class_index);
}

static void
generate_push_argument(DVM_Executable *exe, Block *block,
                       ArgumentList *arg_list, OpcodeBuf *ob)
{
    ArgumentList *arg_pos;

    for (arg_pos = arg_list; arg_pos; arg_pos = arg_pos->next) {
        generate_expression(exe, block, arg_pos->expression, ob);
    }
}

static int
get_method_index(MemberExpression *member)
{
    int method_index;

    if (dkc_is_array(member->expression->type)
        || dkc_is_string(member->expression->type)) {
        method_index = member->method_index;
    } else {
        DBG_assert(member->declaration->kind == METHOD_MEMBER,
                   ("member->declaration->kind..%d",
                    member->declaration->kind));
        method_index = member->declaration->u.method.method_index;
    }

    return method_index;
}

static void
generate_method_call_expression(DVM_Executable *exe, Block *block,
                                Expression *expr, OpcodeBuf *ob)
{
    int method_index;
    MemberExpression *member;

    member = &expr->u.function_call_expression.function->u.member_expression;

    method_index = get_method_index(member);

    generate_push_argument(exe, block,
                           expr->u.function_call_expression.argument, ob);
    generate_expression(exe, block,
                        expr->u.function_call_expression.function
                        ->u.member_expression.expression, ob);
    generate_code(ob, expr->line_number, DVM_PUSH_METHOD, method_index);
    generate_code(ob, expr->line_number, DVM_INVOKE);
}

static void
generate_function_call_expression(DVM_Executable *exe, Block *block,
                                  Expression *expr, OpcodeBuf *ob)
{
    FunctionCallExpression *fce = &expr->u.function_call_expression;

    if (fce->function->kind == MEMBER_EXPRESSION
        && ((dkc_is_array(fce->function->u.member_expression.expression->type)
             || dkc_is_string(fce->function->u.member_expression.expression
                              ->type))
            || (fce->function->u.member_expression.declaration->kind
                == METHOD_MEMBER))) {
        generate_method_call_expression(exe, block, expr, ob);
        return;
    }
    generate_push_argument(exe, block, fce->argument, ob);
    generate_expression(exe, block, fce->function, ob);
    if (dkc_is_delegate(fce->function->type)) {
        generate_code(ob, expr->line_number, DVM_INVOKE_DELEGATE);
    } else {
        generate_code(ob, expr->line_number, DVM_INVOKE);
    }
}

static void
generate_member_expression(DVM_Executable *exe, Block *block,
                           Expression *expr, OpcodeBuf *ob)
{
    MemberDeclaration *member;
    int method_index;
    
    member = expr->u.member_expression.declaration;

    if (dkc_is_array(expr->u.member_expression.expression->type)
        || dkc_is_string(expr->u.member_expression.expression->type)
        || member->kind == METHOD_MEMBER) {
        method_index = get_method_index(&expr->u.member_expression);
        generate_expression(exe, block,
                            expr->u.member_expression.expression, ob);
        generate_code(ob, expr->line_number, DVM_PUSH_METHOD_DELEGATE,
                      method_index);
    } else {
        DBG_assert(member->kind == FIELD_MEMBER,
                   ("member->u.kind..%d", member->kind));
        generate_expression(exe, block,
                            expr->u.member_expression.expression, ob);
        generate_code(ob, expr->line_number,
                      DVM_PUSH_FIELD_INT + get_opcode_type_offset(expr->type),
                      member->u.field.field_index);
    }
}

static void
generate_null_expression(DVM_Executable *exe, Expression *expr,
                         OpcodeBuf *ob)
{
    generate_code(ob, expr->line_number, DVM_PUSH_NULL);
}

static FunctionDefinition *
get_current_function(Block *block)
{
    Block *block_pos;

    for (block_pos = block; block_pos->type != FUNCTION_BLOCK;
         block_pos = block_pos->outer_block)
        ;

    return block_pos->parent.function.function;
}

static void
generate_this_expression(DVM_Executable *exe, Block *block,
                         Expression *expr, OpcodeBuf *ob)
{
    FunctionDefinition *fd;
    int param_count;

    fd = get_current_function(block);
    param_count = count_parameter(fd->parameter);
    generate_code(ob, expr->line_number,
                  DVM_PUSH_STACK_OBJECT, param_count);
}

static void
generate_super_expression(DVM_Executable *exe, Block *block,
                          Expression *expr, OpcodeBuf *ob)
{
    FunctionDefinition *fd;
    int param_count;

    fd = get_current_function(block);
    param_count = count_parameter(fd->parameter);
    generate_code(ob, expr->line_number,
                  DVM_PUSH_STACK_OBJECT, param_count);

    generate_code(ob, expr->line_number, DVM_SUPER);
}

static void
generate_new_expression(DVM_Executable *exe, Block *block,
                        Expression *expr, OpcodeBuf *ob)
{
    int param_count;

    param_count = count_parameter(expr->u.new_e.method_declaration
                                  ->u.method.function_definition->parameter);

    generate_code(ob, expr->line_number, DVM_NEW,
                  expr->u.new_e.class_index);
    generate_push_argument(exe, block, expr->u.new_e.argument, ob);
    generate_code(ob, expr->line_number, DVM_DUPLICATE_OFFSET,
                  param_count);

    generate_code(ob, expr->line_number, DVM_PUSH_METHOD,
                  expr->u.new_e.method_declaration->u.method.method_index);
    generate_code(ob, expr->line_number, DVM_INVOKE);
    generate_code(ob, expr->line_number, DVM_POP);
}

static void
generate_array_creation_expression(DVM_Executable *exe, Block *block,
                                   Expression *expr, OpcodeBuf *ob)
{
    int index;
    TypeSpecifier type;
    ArrayDimension *dim_pos;
    int dim_count;

    index = add_type_specifier(expr->type, exe);

    DBG_assert(expr->type->derive->tag == ARRAY_DERIVE,
               ("expr->type->derive->tag..%d", expr->type->derive->tag));

    type.basic_type = expr->type->basic_type;
    type.derive = expr->type->derive;

    dim_count = 0;
    for (dim_pos = expr->u.array_creation.dimension;
         dim_pos; dim_pos = dim_pos->next) {
        if (dim_pos->expression == NULL)
            break;

        generate_expression(exe, block, dim_pos->expression, ob);
        dim_count++;
    }

    generate_code(ob, expr->line_number, DVM_NEW_ARRAY, dim_count, index);
}

static void
generate_expression(DVM_Executable *exe, Block *current_block,
                    Expression *expr, OpcodeBuf *ob)
{
    switch (expr->kind) {
    case BOOLEAN_EXPRESSION:
        generate_boolean_expression(exe, expr, ob);
        break;
    case INT_EXPRESSION:
        generate_int_expression(exe, expr->line_number, expr->u.int_value,
                                ob);
        break;
    case DOUBLE_EXPRESSION:
        generate_double_expression(exe, expr, ob);
        break;
    case STRING_EXPRESSION:
        generate_string_expression(exe, expr, ob);
        break;
    case IDENTIFIER_EXPRESSION:
        generate_identifier_expression(exe, current_block,
                                       expr, ob);
        break;
    case COMMA_EXPRESSION:
        generate_expression(exe, current_block, expr->u.comma.left, ob);
        generate_expression(exe, current_block, expr->u.comma.right, ob);
        break;
    case ASSIGN_EXPRESSION:
        generate_assign_expression(exe, current_block, expr, ob, DVM_FALSE);
        break;
    case ADD_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_ADD_INT, ob);
        break;
    case SUB_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_SUB_INT, ob);
        break;
    case MUL_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_MUL_INT, ob);
        break;
    case DIV_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_DIV_INT, ob);
        break;
    case MOD_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_MOD_INT, ob);
        break;
    case EQ_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_EQ_INT, ob);
        break;
    case NE_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_NE_INT, ob);
        break;
    case GT_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_GT_INT, ob);
        break;
    case GE_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_GE_INT, ob);
        break;
    case LT_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_LT_INT, ob);
        break;
    case LE_EXPRESSION:
        generate_binary_expression(exe, current_block, expr,
                                   DVM_LE_INT, ob);
        break;
    case BIT_AND_EXPRESSION:
        generate_bit_binary_expression(exe, current_block, expr,
                                       DVM_BIT_AND, ob);
        break;
    case BIT_OR_EXPRESSION:
        generate_bit_binary_expression(exe, current_block, expr,
                                       DVM_BIT_OR, ob);
        break;
    case BIT_XOR_EXPRESSION:
        generate_bit_binary_expression(exe, current_block, expr,
                                       DVM_BIT_XOR, ob);
        break;
    case LOGICAL_AND_EXPRESSION:
        generate_logical_and_expression(exe, current_block, expr, ob);
        break;
    case LOGICAL_OR_EXPRESSION:
        generate_logical_or_expression(exe, current_block, expr, ob);
        break;
    case MINUS_EXPRESSION:
        generate_expression(exe, current_block, expr->u.minus_expression, ob);
        generate_code(ob, expr->line_number,
                      DVM_MINUS_INT
                      + get_opcode_type_offset(expr->type));
        break;
    case BIT_NOT_EXPRESSION:
        generate_expression(exe, current_block, expr->u.bit_not, ob);
        generate_code(ob, expr->line_number, DVM_BIT_NOT);
        break;
    case LOGICAL_NOT_EXPRESSION:
        generate_expression(exe, current_block, expr->u.logical_not, ob);
        generate_code(ob, expr->line_number, DVM_LOGICAL_NOT);
        break;
    case FUNCTION_CALL_EXPRESSION:
        generate_function_call_expression(exe, current_block,
                                          expr, ob);
        break;
    case MEMBER_EXPRESSION:
        generate_member_expression(exe, current_block, expr, ob);
        break;
    case NULL_EXPRESSION:
        generate_null_expression(exe, expr, ob);
        break;
    case THIS_EXPRESSION:
        generate_this_expression(exe, current_block, expr, ob);
        break;
    case SUPER_EXPRESSION:
        generate_super_expression(exe, current_block, expr, ob);
        break;
    case NEW_EXPRESSION:
        generate_new_expression(exe, current_block, expr, ob);
        break;
    case ARRAY_LITERAL_EXPRESSION:
        generate_array_literal_expression(exe, current_block, expr, ob);
        break;
    case INDEX_EXPRESSION:
        generate_index_expression(exe, current_block, expr, ob);
        break;
    case INCREMENT_EXPRESSION:  /* FALLTHRU */
    case DECREMENT_EXPRESSION:
        generate_inc_dec_expression(exe, current_block, expr, expr->kind,
                                    ob, DVM_FALSE);
        break;
    case INSTANCEOF_EXPRESSION:
        generate_instanceof_expression(exe, current_block, expr, ob);
        break;
    case DOWN_CAST_EXPRESSION:
        generate_down_cast_expression(exe, current_block, expr, ob);
        break;
    case CAST_EXPRESSION:
        generate_cast_expression(exe, current_block, expr, ob);
        break;
    case UP_CAST_EXPRESSION:
        generate_up_cast_expression(exe, current_block, expr, ob);
        break;
    case ARRAY_CREATION_EXPRESSION:
        generate_array_creation_expression(exe, current_block, expr, ob);
        break;
    case ENUMERATOR_EXPRESSION:
        generate_int_expression(exe, expr->line_number,
                                expr->u.enumerator.enumerator->value,
                                ob);
        break;
    case EXPRESSION_KIND_COUNT_PLUS_1:  /* FALLTHRU */
    default:
        DBG_assert(0, ("expr->kind..%d", expr->kind));
    }
}

static void
generate_expression_statement(DVM_Executable *exe, Block *block,
                              Expression *expr, OpcodeBuf *ob)
{
    if (expr->kind == ASSIGN_EXPRESSION) {
        generate_assign_expression(exe, block, expr, ob, DVM_TRUE);
    } else if (expr->kind == INCREMENT_EXPRESSION
               || expr->kind == DECREMENT_EXPRESSION) {
        generate_inc_dec_expression(exe, block, expr, expr->kind, ob,
                                    DVM_TRUE);
    } else {
        generate_expression(exe, block, expr, ob);
        generate_code(ob, expr->line_number, DVM_POP);
    }
}

static void generate_statement_list(DVM_Executable *exe, Block *current_block,
                                    StatementList *statement_list,
                                    OpcodeBuf *ob);

static void
generate_if_statement(DVM_Executable *exe, Block *block,
                      Statement *statement, OpcodeBuf *ob)
{
    int if_false_label;
    int end_label;
    IfStatement *if_s = &statement->u.if_s;
    Elsif *elsif;

    generate_expression(exe, block, if_s->condition, ob);
    if_false_label = get_label(ob);
    generate_code(ob, statement->line_number,
                  DVM_JUMP_IF_FALSE, if_false_label);
    generate_statement_list(exe, if_s->then_block,
                            if_s->then_block->statement_list, ob);
    end_label = get_label(ob);
    generate_code(ob, statement->line_number, DVM_JUMP, end_label);
    set_label(ob, if_false_label);

    for (elsif = if_s->elsif_list; elsif; elsif = elsif->next) {
        generate_expression(exe, block, elsif->condition, ob);
        if_false_label = get_label(ob);
        generate_code(ob, statement->line_number,
                      DVM_JUMP_IF_FALSE, if_false_label);
        generate_statement_list(exe, elsif->block,
                                elsif->block->statement_list, ob);
        generate_code(ob, statement->line_number, DVM_JUMP, end_label);
        set_label(ob, if_false_label);
    }
    if (if_s->else_block) {
        generate_statement_list(exe, if_s->else_block,
                                if_s->else_block->statement_list,
                                ob);
    }
    set_label(ob, end_label);
}

static void
generate_switch_statement(DVM_Executable *exe, Block *block,
                          Statement *statement, OpcodeBuf *ob)
{
    SwitchStatement *switch_s = &statement->u.switch_s;
    CaseList *case_pos;
    ExpressionList *expr_pos;
    int offset;
    int case_start_label;
    int next_case_label;
    int end_label;
    int line_number;

    generate_expression(exe, block, switch_s->expression, ob);

    end_label = get_label(ob);

    for (case_pos = switch_s->case_list; case_pos;
         case_pos = case_pos->next) {

        case_start_label = get_label(ob);
        for (expr_pos = case_pos->expression_list; expr_pos;
             expr_pos = expr_pos->next) {
            generate_code(ob, statement->line_number,
                          DVM_DUPLICATE);
            generate_expression(exe, block, expr_pos->expression, ob);
            offset = get_binary_expression_offset(switch_s->expression,
                                                  expr_pos->expression,
                                                  DVM_EQ_INT);
            generate_code(ob, expr_pos->expression->line_number,
                          DVM_EQ_INT + offset);
            generate_code(ob, expr_pos->expression->line_number,
                          DVM_JUMP_IF_TRUE, case_start_label);
            line_number = expr_pos->expression->line_number;
        }
        next_case_label = get_label(ob);
        generate_code(ob, line_number, DVM_JUMP, next_case_label);
        set_label(ob, case_start_label);
        generate_statement_list(exe, case_pos->block,
                                case_pos->block->statement_list, ob);
        generate_code(ob, statement->line_number, DVM_JUMP, end_label);
        set_label(ob, next_case_label);
    }
    if (switch_s->default_block) {
        generate_statement_list(exe, switch_s->default_block,
                                switch_s->default_block->statement_list, ob);
    }

    set_label(ob, end_label);
    generate_code(ob, statement->line_number, DVM_POP);
}

static void
generate_while_statement(DVM_Executable *exe, Block *block,
                         Statement *statement, OpcodeBuf *ob)
{
    int loop_label;
    WhileStatement *while_s = &statement->u.while_s;

    loop_label = get_label(ob);
    set_label(ob, loop_label);

    generate_expression(exe, block, while_s->condition, ob);

    while_s->block->parent.statement.break_label = get_label(ob);
    while_s->block->parent.statement.continue_label = get_label(ob);

    generate_code(ob, statement->line_number,
                  DVM_JUMP_IF_FALSE,
                  while_s->block->parent.statement.break_label);
    generate_statement_list(exe, while_s->block,
                            while_s->block->statement_list, ob);

    set_label(ob, while_s->block->parent.statement.continue_label);
    generate_code(ob, statement->line_number, DVM_JUMP, loop_label);
    set_label(ob, while_s->block->parent.statement.break_label);
}

static void
generate_for_statement(DVM_Executable *exe, Block *block,
                       Statement *statement, OpcodeBuf *ob)
{
    int loop_label;
    ForStatement *for_s = &statement->u.for_s;

    if (for_s->init) {
        generate_expression_statement(exe, block, for_s->init, ob);
    }
    loop_label = get_label(ob);
    set_label(ob, loop_label);

    if (for_s->condition) {
        generate_expression(exe, block, for_s->condition, ob);
    }

    for_s->block->parent.statement.break_label = get_label(ob);
    for_s->block->parent.statement.continue_label = get_label(ob);

    if (for_s->condition) {
        generate_code(ob, statement->line_number,
                      DVM_JUMP_IF_FALSE,
                      for_s->block->parent.statement.break_label);
    }

    generate_statement_list(exe, for_s->block,
                            for_s->block->statement_list, ob);
    set_label(ob, for_s->block->parent.statement.continue_label);

    if (for_s->post) {
        generate_expression_statement(exe, block, for_s->post, ob);
    }

    generate_code(ob, statement->line_number,
                  DVM_JUMP, loop_label);
    set_label(ob, for_s->block->parent.statement.break_label);
}

static void
generate_do_while_statement(DVM_Executable *exe, Block *block,
                            Statement *statement, OpcodeBuf *ob)
{
    int loop_label;
    DoWhileStatement *do_while_s = &statement->u.do_while_s;

    loop_label = get_label(ob);
    set_label(ob, loop_label);

    do_while_s->block->parent.statement.break_label = get_label(ob);
    do_while_s->block->parent.statement.continue_label = get_label(ob);

    generate_statement_list(exe, do_while_s->block,
                            do_while_s->block->statement_list, ob);

    set_label(ob, do_while_s->block->parent.statement.continue_label);
    generate_expression(exe, block, do_while_s->condition, ob);
    generate_code(ob, statement->line_number,
                  DVM_JUMP_IF_TRUE, loop_label);
    set_label(ob, do_while_s->block->parent.statement.break_label);
}

static void
generate_return_statement(DVM_Executable *exe, Block *block,
                          Statement *statement, OpcodeBuf *ob)
{
    DKC_Compiler *compiler = dkc_get_current_compiler();
    Block       *block_p;

    DBG_assert(statement->u.return_s.return_value != NULL,
               ("return value is null."));

    for (block_p = block; block_p; block_p = block_p->outer_block) {
        if (block_p->type == TRY_CLAUSE_BLOCK
            || block_p->type == CATCH_CLAUSE_BLOCK) {
            generate_code(ob, statement->line_number,
                          DVM_GO_FINALLY, compiler->current_finally_label);
        }
    }
    generate_expression(exe, block, statement->u.return_s.return_value, ob);
    generate_code(ob, statement->line_number, DVM_RETURN);
}

static void
generate_break_statement(DVM_Executable *exe, Block *block,
                         Statement *statement, OpcodeBuf *ob)
{
    BreakStatement *break_s = &statement->u.break_s;
    Block       *block_p;
    DVM_Boolean finally_flag = DVM_FALSE;

    for (block_p = block; block_p; block_p = block_p->outer_block) {
        if (block_p->type == TRY_CLAUSE_BLOCK
            || block_p->type == CATCH_CLAUSE_BLOCK) {
            finally_flag = DVM_TRUE;
        }
        if (block_p->type != WHILE_STATEMENT_BLOCK
            && block_p->type != FOR_STATEMENT_BLOCK
            && block_p->type != DO_WHILE_STATEMENT_BLOCK)
            continue;

        if (break_s->label == NULL) {
            break;
        }

        if (block_p->type == WHILE_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.while_s.label == NULL)
                continue;

            if (!strcmp(break_s->label,
                        block_p->parent.statement.statement
                        ->u.while_s.label)) {
                break;
            }
        } else if (block_p->type == FOR_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.for_s.label == NULL)
                continue;

            if (!strcmp(break_s->label,
                        block_p->parent.statement.statement
                        ->u.for_s.label)) {
                break;
            }
        } else if (block_p->type == DO_WHILE_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.do_while_s.label
                == NULL)
                continue;

            if (!strcmp(break_s->label,
                        block_p->parent.statement.statement
                        ->u.do_while_s.label)) {
                break;
            }
        }
    }
    if (block_p == NULL) {
        dkc_compile_error(statement->line_number,
                          LABEL_NOT_FOUND_ERR,
                          STRING_MESSAGE_ARGUMENT, "label", break_s->label,
                          MESSAGE_ARGUMENT_END);
    }

    if (finally_flag) {
        DKC_Compiler *compiler = dkc_get_current_compiler();
        generate_code(ob, statement->line_number,
                      DVM_GO_FINALLY, compiler->current_finally_label);
    }
    generate_code(ob, statement->line_number,
                  DVM_JUMP,
                  block_p->parent.statement.break_label);

}

static void
generate_continue_statement(DVM_Executable *exe, Block *block,
                            Statement *statement, OpcodeBuf *ob)
{
    ContinueStatement *continue_s = &statement->u.continue_s;
    Block       *block_p;
    DVM_Boolean finally_flag = DVM_FALSE;

    for (block_p = block; block_p; block_p = block_p->outer_block) {
        if (block_p->type == TRY_CLAUSE_BLOCK
            || block_p->type == CATCH_CLAUSE_BLOCK) {
            finally_flag = DVM_TRUE;
        }
        if (block_p->type != WHILE_STATEMENT_BLOCK
            && block_p->type != FOR_STATEMENT_BLOCK
            && block_p->type != DO_WHILE_STATEMENT_BLOCK)
            continue;

        if (continue_s->label == NULL) {
            break;
        }

        if (block_p->type == WHILE_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.while_s.label == NULL)
                continue;

            if (!strcmp(continue_s->label,
                        block_p->parent.statement.statement
                        ->u.while_s.label)) {
                break;
            }
        } else if (block_p->type == FOR_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.for_s.label == NULL)
                continue;

            if (!strcmp(continue_s->label,
                        block_p->parent.statement.statement
                        ->u.for_s.label)) {
                break;
            }
        } else if (block_p->type == DO_WHILE_STATEMENT_BLOCK) {
            if (block_p->parent.statement.statement->u.do_while_s.label
                == NULL)
                continue;

            if (!strcmp(continue_s->label,
                        block_p->parent.statement.statement
                        ->u.do_while_s.label)) {
                break;
            }
        }
    }
    if (block_p == NULL) {
        dkc_compile_error(statement->line_number,
                          LABEL_NOT_FOUND_ERR,
                          STRING_MESSAGE_ARGUMENT, "label", continue_s->label,
                          MESSAGE_ARGUMENT_END);
    }
    if (finally_flag) {
        DKC_Compiler *compiler = dkc_get_current_compiler();
        generate_code(ob, statement->line_number,
                      DVM_GO_FINALLY, compiler->current_finally_label);
    }
    generate_code(ob, statement->line_number,
                  DVM_JUMP,
                  block_p->parent.statement.continue_label);
}

static void
add_try_to_opcode_buf(OpcodeBuf *ob, DVM_Try *try)
{
    ob->try = MEM_realloc(ob->try, sizeof(DVM_Try) * (ob->try_size+1));
    ob->try[ob->try_size] = *try;
    ob->try_size++;
}

static void
generate_try_statement(DVM_Executable *exe, Block *block,
                       Statement *statement, OpcodeBuf *ob)
{
    TryStatement *try_s = &statement->u.try_s;
    CatchClause *catch_pos;
    DVM_Try dvm_try;
    int catch_count = 0;
    int catch_index;
    DVM_CatchClause *dvm_catch;
    int after_finally_label;
    int finally_label_backup;
    DKC_Compiler *compiler = dkc_get_current_compiler();

    after_finally_label = get_label(ob);
    finally_label_backup = compiler->current_finally_label;
    compiler->current_finally_label = get_label(ob);
    dvm_try.try_start_pc = ob->size;
    generate_statement_list(exe, try_s->try_block,
                            try_s->try_block->statement_list, ob);
    generate_code(ob, statement->line_number,
                  DVM_GO_FINALLY, compiler->current_finally_label);
    generate_code(ob, statement->line_number,
                  DVM_JUMP, after_finally_label);
    dvm_try.try_end_pc = ob->size-1;

    for (catch_pos = try_s->catch_clause; catch_pos;
         catch_pos = catch_pos->next) {
        catch_count++;
    }
    dvm_catch = MEM_malloc(sizeof(DVM_CatchClause) * catch_count);

    for (catch_pos = try_s->catch_clause, catch_index = 0;
         catch_pos;
         catch_pos = catch_pos->next, catch_index++) {
        dvm_catch[catch_index].class_index
            = catch_pos->type->u.class_ref.class_index;
        dvm_catch[catch_index].start_pc = ob->size;

        generate_pop_to_identifier(catch_pos->variable_declaration,
                                   catch_pos->line_number, ob);
        generate_statement_list(exe, catch_pos->block,
                                catch_pos->block->statement_list, ob);
        generate_code(ob, catch_pos->line_number,
                      DVM_GO_FINALLY, compiler->current_finally_label);
        generate_code(ob, catch_pos->line_number,
                      DVM_JUMP, after_finally_label);
        dvm_catch[catch_index].end_pc = ob->size-1;
    }
    dvm_try.catch_clause = dvm_catch;
    dvm_try.catch_count = catch_count;
    
    dvm_try.finally_start_pc = ob->size;
    set_label(ob, compiler->current_finally_label);
    if (try_s->finally_block) {
        generate_statement_list(exe, try_s->finally_block,
                                try_s->finally_block->statement_list, ob);
    }
    generate_code(ob, statement->line_number,
                  DVM_FINALLY_END);
    set_label(ob, after_finally_label);
    dvm_try.finally_end_pc = ob->size-1;

    add_try_to_opcode_buf(ob, &dvm_try);
}

static void
generate_throw_statement(DVM_Executable *exe, Block *block,
                         Statement *statement, OpcodeBuf *ob)
{
    if (statement->u.throw_s.exception) {
        generate_expression(exe, block, statement->u.throw_s.exception, ob);
        generate_code(ob, statement->line_number, DVM_THROW);
    } else {
        generate_identifier(statement->u.throw_s.variable_declaration, ob,
                            statement->line_number);
        generate_code(ob, statement->line_number, DVM_RETHROW);
    }
}

static void
generate_initializer(DVM_Executable *exe, Block *block,
                     Statement *statement, OpcodeBuf *ob)
{
    Declaration *decl = statement->u.declaration_s;
    if (decl->initializer == NULL)
        return;

    generate_expression(exe, block, decl->initializer, ob);
    generate_pop_to_identifier(decl, statement->line_number,
                               ob);
}

static void
generate_statement_list(DVM_Executable *exe, Block *current_block,
                        StatementList *statement_list,
                        OpcodeBuf *ob)
{
    StatementList *pos;

    for (pos = statement_list; pos; pos = pos->next) {
        switch (pos->statement->type) {
        case EXPRESSION_STATEMENT:
            generate_expression_statement(exe, current_block,
                                          pos->statement->u.expression_s, ob);
            break;
        case IF_STATEMENT:
            generate_if_statement(exe, current_block, pos->statement, ob);
            break;
        case SWITCH_STATEMENT:
            generate_switch_statement(exe, current_block, pos->statement, ob);
            break;
        case WHILE_STATEMENT:
            generate_while_statement(exe, current_block, pos->statement, ob);
            break;
        case FOR_STATEMENT:
            generate_for_statement(exe, current_block, pos->statement, ob);
            break;
        case DO_WHILE_STATEMENT:
            generate_do_while_statement(exe, current_block,
                                        pos->statement, ob);
            break;
        case FOREACH_STATEMENT:
            break;
        case RETURN_STATEMENT:
            generate_return_statement(exe, current_block, pos->statement, ob);
            break;
        case BREAK_STATEMENT:
            generate_break_statement(exe, current_block, pos->statement, ob);
            break;
        case CONTINUE_STATEMENT:
            generate_continue_statement(exe, current_block,
                                        pos->statement, ob);
            break;
        case TRY_STATEMENT:
            generate_try_statement(exe, current_block, pos->statement, ob);
            break;
        case THROW_STATEMENT:
            generate_throw_statement(exe, current_block, pos->statement, ob);
            break;
        case DECLARATION_STATEMENT:
            generate_initializer(exe, current_block,
                                 pos->statement, ob);
            break;
        case STATEMENT_TYPE_COUNT_PLUS_1: /* FALLTHRU */
        default:
            DBG_assert(0, ("pos->statement->type..", pos->statement->type));
        }
    }
}


static int
search_function(DKC_Compiler *compiler, FunctionDefinition *src)
{
    int i;
    char *src_package_name;
    char *func_name;

    src_package_name = dkc_package_name_to_string(src->package_name);
    if (src->class_definition) {
        func_name
            = dvm_create_method_function_name(src->class_definition->name,
                                              src->name);
    } else {
        func_name = src->name;
    }
    for (i = 0; i < compiler->dvm_function_count; i++) {
        if (dvm_compare_package_name(src_package_name,
                                     compiler->dvm_function[i].package_name)
            && !strcmp(func_name, compiler->dvm_function[i].name)) {
            MEM_free(src_package_name);
            if (src->class_definition) {
                MEM_free(func_name);
            }
            return i;
        }
    }
    DBG_assert(0, ("function %s::%s not found.", src_package_name, src->name));

    return 0; /* make compiler happy */
}

static DVM_LocalVariable *
copy_local_variables(FunctionDefinition *fd, int param_count)
{
    int i;
    int local_variable_count;
    DVM_LocalVariable *dest;

    local_variable_count = fd->local_variable_count - param_count;

    dest = MEM_malloc(sizeof(DVM_LocalVariable) * local_variable_count);

    for (i = 0; i < local_variable_count; i++) {
        dest[i].name
            = MEM_strdup(fd->local_variable[i+param_count]->name);
        dest[i].type
            = dkc_copy_type_specifier(fd->local_variable[i+param_count]->type);
    }

    return dest;
}

static void
add_function(DVM_Executable *exe,
             FunctionDefinition *src, DVM_Function *dest,
             DVM_Boolean in_this_exe)
{
    OpcodeBuf           ob;

    dest->type = dkc_copy_type_specifier(src->type);
    dest->parameter = copy_parameter_list(src->parameter,
                                          &dest->parameter_count);

    if (src->block && in_this_exe) {
        init_opcode_buf(&ob);
        generate_statement_list(exe, src->block, src->block->statement_list,
                                &ob);

        dest->is_implemented = DVM_TRUE;
        dest->code_block.code_size = ob.size;
        dest->code_block.code = fix_opcode_buf(&ob);
        dest->code_block.line_number_size = ob.line_number_size;
        dest->code_block.line_number = ob.line_number;
        dest->code_block.line_number = ob.line_number;
        dest->code_block.try_size = ob.try_size;
        dest->code_block.try = ob.try;
        dest->code_block.need_stack_size
            = calc_need_stack_size(dest->code_block.code,
                                   dest->code_block.code_size);
        dest->local_variable
            = copy_local_variables(src, dest->parameter_count);
        dest->local_variable_count
            = src->local_variable_count - dest->parameter_count;
    } else {
        dest->is_implemented = DVM_FALSE;
        dest->local_variable = NULL;
        dest->local_variable_count = 0;
    }
    if (src->class_definition) {
        dest->is_method = DVM_TRUE;
    } else {
        dest->is_method = DVM_FALSE;
    }
}

static void
add_functions(DKC_Compiler *compiler, DVM_Executable *exe)
{
    FunctionDefinition  *fd;
    int dest_idx;
    int i;
    DVM_Boolean *in_this_exe;

    in_this_exe = MEM_malloc(sizeof(DVM_Boolean)
                                     * compiler->dvm_function_count);
    for (i = 0; i < compiler->dvm_function_count; i++) {
        in_this_exe[i] = DVM_FALSE;
    }
    for (fd = compiler->function_list; fd; fd = fd->next) {
        if (fd->class_definition && fd->block == NULL)
            continue;

        dest_idx = search_function(compiler, fd);
        in_this_exe[dest_idx] = DVM_TRUE;
        add_function(exe, fd, &compiler->dvm_function[dest_idx], DVM_TRUE);
    }

    for (i = 0; i < compiler->dvm_function_count; i++) {
        if (in_this_exe[i])
            continue;
        fd = dkc_search_function(compiler->dvm_function[i].name);
        add_function(exe, fd, &compiler->dvm_function[i], DVM_FALSE);
    }
    MEM_free(in_this_exe);
}

static void
add_top_level(DKC_Compiler *compiler, DVM_Executable *exe)
{
    OpcodeBuf           ob;

    init_opcode_buf(&ob);
    generate_statement_list(exe, NULL, compiler->statement_list,
                            &ob);
    
    exe->top_level.code_size = ob.size;
    exe->top_level.code = fix_opcode_buf(&ob);
    exe->top_level.line_number_size = ob.line_number_size;
    exe->top_level.line_number = ob.line_number;
    exe->top_level.try_size = ob.try_size;
    exe->top_level.try = ob.try;
    exe->top_level.need_stack_size
        = calc_need_stack_size(exe->top_level.code, exe->top_level.code_size);
}

static void
generate_constant_initializer(DKC_Compiler *compiler, DVM_Executable *exe)
{
    ConstantDefinition *cd_pos;
    OpcodeBuf           ob;

    init_opcode_buf(&ob);
    for (cd_pos = compiler->constant_definition_list; cd_pos;
         cd_pos = cd_pos->next) {
        if (cd_pos->initializer) {
            generate_expression(exe, NULL, cd_pos->initializer, &ob);
            generate_code(&ob, cd_pos->line_number,
                          DVM_POP_CONSTANT_INT
                          + get_opcode_type_offset(cd_pos->type),
                          cd_pos->index);
        }
    }
    /* BUGBUG use copy_opcode_buf() */
    exe->constant_initializer.code_size = ob.size;
    exe->constant_initializer.code = fix_opcode_buf(&ob);
    exe->constant_initializer.line_number_size = ob.line_number_size;
    exe->constant_initializer.line_number = ob.line_number;
    exe->constant_initializer.try_size = ob.try_size;
    exe->constant_initializer.try = ob.try;
    exe->constant_initializer.need_stack_size
        = calc_need_stack_size(exe->constant_initializer.code,
                               exe->constant_initializer.code_size);

}

DVM_Executable *
dkc_generate(DKC_Compiler *compiler)
{
    DVM_Executable      *exe;

    exe = alloc_executable(compiler->package_name);

    exe->function_count = compiler->dvm_function_count;
    exe->function = compiler->dvm_function;
    exe->class_count = compiler->dvm_class_count;
    exe->class_definition = compiler->dvm_class;
    exe->enum_count = compiler->dvm_enum_count;
    exe->enum_definition = compiler->dvm_enum;
    exe->constant_count = compiler->dvm_constant_count;
    exe->constant_definition = compiler->dvm_constant;

    add_global_variable(compiler, exe);
    add_classes(compiler, exe);
    add_functions(compiler, exe);
    add_top_level(compiler, exe);

    generate_constant_initializer(compiler, exe);

    return exe;
}
