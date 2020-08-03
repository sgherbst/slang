//------------------------------------------------------------------------------
// MiscExpressions.cpp
// Definitions for miscellaneous expressions
//
// File is under the MIT license; see LICENSE for details
//------------------------------------------------------------------------------
#include "slang/binding/MiscExpressions.h"

#include "slang/binding/LiteralExpressions.h"
#include "slang/binding/SystemSubroutine.h"
#include "slang/compilation/Compilation.h"
#include "slang/diagnostics/ConstEvalDiags.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/symbols/ASTSerializer.h"
#include "slang/symbols/MemberSymbols.h"
#include "slang/symbols/ParameterSymbols.h"
#include "slang/symbols/Type.h"
#include "slang/symbols/VariableSymbols.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/util/StackContainer.h"

namespace {

using namespace slang;

const Type& getIndexedType(Compilation& compilation, const BindContext& context,
                           const Type& valueType, SourceRange exprRange, SourceRange valueRange,
                           bool isRangeSelect) {
    const Type& ct = valueType.getCanonicalType();
    if (ct.isArray()) {
        return *ct.getArrayElementType();
    }
    else if (ct.kind == SymbolKind::StringType && !isRangeSelect) {
        return compilation.getByteType();
    }
    else if (!ct.isIntegral()) {
        auto& diag = context.addDiag(diag::BadIndexExpression, exprRange);
        diag << valueRange;
        diag << valueType;
        return compilation.getErrorType();
    }
    else if (ct.isScalar()) {
        auto& diag = context.addDiag(diag::CannotIndexScalar, exprRange);
        diag << valueRange;
        return compilation.getErrorType();
    }
    else if (ct.isFourState()) {
        return compilation.getLogicType();
    }
    else {
        return compilation.getBitType();
    }
}

optional<ConstantRange> getFixedIndex(const ConstantValue& cs, const Type& valueType,
                                      const Type& resultType, EvalContext& context,
                                      SourceRange sourceRange) {
    optional<int32_t> index = cs.integer().as<int32_t>();
    ConstantRange range = valueType.getFixedRange();
    if (!index || !range.containsPoint(*index)) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cs << valueType;
        return std::nullopt;
    }

    if (valueType.isUnpackedArray()) {
        // Unpacked arrays are stored reversed in memory, so reverse the range here.
        range = range.reverse();
        int32_t i = range.translateIndex(*index);
        return ConstantRange{ i, i };
    }

    // For packed arrays, we're selecting bit ranges, not necessarily single bits,
    // so multiply out by the width of each element.
    int32_t width = (int32_t)resultType.getBitWidth();
    int32_t i = range.translateIndex(*index) * width;
    return ConstantRange{ i + width - 1, i };
}

optional<int32_t> getDynamicIndex(const ConstantValue& cs, const ConstantValue& cv,
                                  const Type& valueType, EvalContext& context,
                                  SourceRange sourceRange) {
    optional<int32_t> index = cs.integer().as<int32_t>();
    if (valueType.isString()) {
        const std::string& str = cv.str();
        if (!index || *index < 0 || size_t(*index) >= str.size()) {
            context.addDiag(diag::ConstEvalStringIndexInvalid, sourceRange) << cs << str.size();
            return std::nullopt;
        }

        return index;
    }

    // For dynamic arrays and queues, elements out of bounds only issue a warning.
    size_t maxIndex = cv.size();
    if (cv.isQueue())
        maxIndex++;

    if (!index || *index < 0 || size_t(*index) >= maxIndex) {
        context.addDiag(diag::ConstEvalDynamicArrayIndex, sourceRange)
            << cs << valueType << maxIndex;

        // Return a sentinel value (which is never valid as a dynamic array index).
        return -1;
    }

    return index;
}

} // namespace

namespace slang {

Expression& NamedValueExpression::fromSymbol(const BindContext& context, const Symbol& symbol,
                                             bool isHierarchical, SourceRange sourceRange) {
    Compilation& compilation = context.getCompilation();
    if (!symbol.isValue()) {
        context.addDiag(diag::NotAValue, sourceRange) << symbol.name;
        return badExpr(compilation, nullptr);
    }

    if ((context.flags & BindFlags::StaticInitializer) != 0 &&
        VariableSymbol::isKind(symbol.kind) &&
        symbol.as<VariableSymbol>().lifetime == VariableLifetime::Automatic) {
        context.addDiag(diag::AutoFromStaticInit, sourceRange) << symbol.name;
        return badExpr(compilation, nullptr);
    }

    return *compilation.emplace<NamedValueExpression>(symbol.as<ValueSymbol>(), isHierarchical,
                                                      sourceRange);
}

ConstantValue NamedValueExpression::evalImpl(EvalContext& context) const {
    if (!verifyConstantImpl(context))
        return nullptr;

    switch (symbol.kind) {
        case SymbolKind::Parameter:
            return symbol.as<ParameterSymbol>().getValue();
        case SymbolKind::EnumValue:
            return symbol.as<EnumValueSymbol>().getValue();
        default:
            ConstantValue* v = context.findLocal(&symbol);
            if (v)
                return *v;
            break;
    }

    // If we reach this point, the variable was not found, which should mean that
    // it's not actually constant.
    auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
    diag.addNote(diag::NoteDeclarationHere, symbol.location);
    return nullptr;
}

LValue NamedValueExpression::evalLValueImpl(EvalContext& context) const {
    if (!verifyConstantImpl(context))
        return nullptr;

    auto cv = context.findLocal(&symbol);
    if (!cv) {
        auto& diag = context.addDiag(diag::ConstEvalNonConstVariable, sourceRange) << symbol.name;
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        return nullptr;
    }

    return LValue(*cv);
}

bool NamedValueExpression::verifyConstantImpl(EvalContext& context) const {
    if (context.isScriptEval())
        return true;

    // Hierarchical names are disallowed in constant expressions and constant functions
    if (isHierarchical) {
        context.addDiag(diag::ConstEvalHierarchicalNameInCE, sourceRange) << symbol.name;
        return false;
    }

    const EvalContext::Frame& frame = context.topFrame();
    const SubroutineSymbol* subroutine = frame.subroutine;
    if (!subroutine)
        return true;

    // Constant functions have a bunch of additional restrictions. See [13.4.4]:
    // - All parameter values used within the function shall be defined before the use of
    //   the invoking constant function call.
    // - All identifiers that are not parameters or functions shall be declared locally to
    //   the current function.
    if (symbol.kind != SymbolKind::Parameter && symbol.kind != SymbolKind::EnumValue) {
        const Scope* scope = symbol.getParentScope();
        while (scope && scope != subroutine)
            scope = scope->asSymbol().getParentScope();

        if (scope != subroutine) {
            auto& diag =
                context.addDiag(diag::ConstEvalFunctionIdentifiersMustBeLocal, sourceRange);
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            return false;
        }
    }
    else {
        // If the two locations are not in the same compilation unit, assume that it's ok.
        auto compare = symbol.isDeclaredBefore(frame.lookupLocation);
        if (!compare.value_or(true)) {
            auto& diag = context.addDiag(diag::ConstEvalIdUsedInCEBeforeDecl, sourceRange)
                         << symbol.name;
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            return false;
        }
    }

    return true;
}

bool NamedValueExpression::verifyAssignableImpl(const BindContext& context, bool isNonBlocking,
                                                SourceLocation location) const {
    if (symbol.kind == SymbolKind::Parameter || symbol.kind == SymbolKind::EnumValue) {
        auto& diag = context.addDiag(diag::ExpressionNotAssignable, location);
        diag.addNote(diag::NoteDeclarationHere, symbol.location);
        diag << sourceRange;
        return false;
    }

    if (VariableSymbol::isKind(symbol.kind)) {
        auto& var = symbol.as<VariableSymbol>();
        if (var.isConstant) {
            auto& diag = context.addDiag(diag::AssignmentToConst, location);
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            diag << symbol.name << sourceRange;
            return false;
        }

        if (isNonBlocking && var.lifetime == VariableLifetime::Automatic) {
            auto& diag = context.addDiag(diag::NonblockingAssignmentToAuto, location);
            diag.addNote(diag::NoteDeclarationHere, symbol.location);
            diag << symbol.name << sourceRange;
            return false;
        }
    }

    return true;
}

void NamedValueExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("symbol", symbol);
    serializer.write("isHierarchical", isHierarchical);
}

Expression& ElementSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                                const ExpressionSyntax& syntax,
                                                SourceRange fullRange, const BindContext& context) {
    if (value.bad())
        return badExpr(compilation, nullptr);

    const Type& valueType = *value.type;
    const Type& resultType = getIndexedType(compilation, context, valueType, syntax.sourceRange(),
                                            value.sourceRange, false);

    // If this is an associative array with a specific index target, we need to bind
    // as an rvalue to get the right conversion applied.
    const Expression* selector = nullptr;
    if (valueType.isAssociativeArray()) {
        auto indexType = valueType.getAssociativeIndexType();
        if (indexType)
            selector = &bindRValue(*indexType, syntax, syntax.getFirstToken().location(), context);
    }

    if (!selector) {
        selector = &selfDetermined(compilation, syntax, context);
        if (!selector->bad() && !selector->type->isIntegral()) {
            context.addDiag(diag::ExprMustBeIntegral, selector->sourceRange);
            return badExpr(compilation, nullptr);
        }
    }

    auto result =
        compilation.emplace<ElementSelectExpression>(resultType, value, *selector, fullRange);
    if (selector->bad() || result->bad())
        return badExpr(compilation, result);

    // If the selector is constant, and the underlying type has a fixed range,
    // we can do checking at compile time that it's within bounds.
    // Only do that if we're not in an unevaluated conditional branch.
    if (valueType.hasFixedRange()) {
        ConstantValue selVal;
        if (!context.inUnevaluatedBranch() && (selVal = context.tryEval(*selector))) {
            optional<int32_t> index = selVal.integer().as<int32_t>();
            if (!index || !valueType.getFixedRange().containsPoint(*index)) {
                auto& diag = context.addDiag(diag::IndexValueInvalid, selector->sourceRange);
                diag << selVal;
                diag << *value.type;
                return badExpr(compilation, result);
            }
        }
    }

    return *result;
}

Expression& ElementSelectExpression::fromConstant(Compilation& compilation, Expression& value,
                                                  int32_t index, const BindContext& context) {
    Expression* indexExpr = &IntegerLiteral::fromConstant(compilation, index);
    selfDetermined(context, indexExpr);

    const Type& resultType = getIndexedType(compilation, context, *value.type,
                                            indexExpr->sourceRange, value.sourceRange, false);

    auto result = compilation.emplace<ElementSelectExpression>(resultType, value, *indexExpr,
                                                               value.sourceRange);
    if (value.bad() || indexExpr->bad() || result->bad())
        return badExpr(compilation, result);

    return *result;
}

ConstantValue ElementSelectExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    ConstantValue cs = selector().eval(context);
    if (!cv || !cs)
        return nullptr;

    // Handling for packed and unpacked arrays, all integer types.
    const Type& valType = *value().type;
    if (valType.hasFixedRange()) {
        auto range = getFixedIndex(cs, valType, *type, context, sourceRange);
        if (!range)
            return nullptr;

        // For fixed types, we know we will always be in range, so just do the selection.
        if (valType.isUnpackedArray())
            return cv.elements()[size_t(range->left)];
        else
            return cv.integer().slice(range->left, range->right);
    }

    // Handling for associative arrays.
    if (valType.isAssociativeArray()) {
        if (cs.hasUnknown()) {
            context.addDiag(diag::ConstEvalAssociativeIndexInvalid, selector().sourceRange) << cs;
            return nullptr;
        }

        auto& map = *cv.map();
        if (auto it = map.find(cs); it != map.end())
            return it->second;

        // If there is a user specified default, return that without warning.
        if (map.defaultValue)
            return map.defaultValue;

        // Otherwise issue a warning and use the default default.
        context.addDiag(diag::ConstEvalAssociativeElementNotFound, selector().sourceRange)
            << value().sourceRange << cs;
        return type->getDefaultValue();
    }

    // Handling for strings, dynamic arrays, and queues.
    auto index = getDynamicIndex(cs, cv, valType, context, sourceRange);
    if (!index)
        return nullptr;

    if (valType.isString())
        return cv.getSlice(*index, *index, nullptr);

    // -1 is returned for dynamic array indices that are out of bounds.
    if (*index == -1)
        return type->getDefaultValue();

    return std::move(cv).at(size_t(*index));
}

LValue ElementSelectExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    ConstantValue cs = selector().eval(context);
    if (!lval || !cs)
        return nullptr;

    // Handling for packed and unpacked arrays, all integer types.
    const Type& valType = *value().type;
    if (valType.hasFixedRange()) {
        auto range = getFixedIndex(cs, valType, *type, context, sourceRange);
        if (!range)
            return nullptr;

        // For fixed types, we know we will always be in range, so just do the selection.
        if (valType.isUnpackedArray())
            lval.addIndex(range->left, type->getDefaultValue());
        else
            lval.addBitSlice(*range);
        return lval;
    }

    // Handling for associative arrays.
    if (valType.isAssociativeArray()) {
        if (cs.hasUnknown()) {
            context.addDiag(diag::ConstEvalAssociativeIndexInvalid, selector().sourceRange) << cs;
            return nullptr;
        }

        lval.addArrayLookup(std::move(cs), type->getDefaultValue());
        return lval;
    }

    // Handling for strings, dynamic arrays, and queues.
    auto index = getDynamicIndex(cs, lval.load(), valType, context, sourceRange);
    if (!index)
        return nullptr;

    if (valType.isString()) {
        lval.addIndex(*index, nullptr);
    }
    else {
        // -1 is returned for dynamic array indices that are out of bounds.
        // LValue handles selecting elements out of bounds and ignores accesses to those locations.
        lval.addIndex(*index, type->getDefaultValue());
    }
    return lval;
}

bool ElementSelectExpression::verifyConstantImpl(EvalContext& context) const {
    return value().verifyConstant(context) && selector().verifyConstant(context);
}

void ElementSelectExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("value", value());
    serializer.write("selector", selector());
}

Expression& RangeSelectExpression::fromSyntax(Compilation& compilation, Expression& value,
                                              const RangeSelectSyntax& syntax,
                                              SourceRange fullRange, const BindContext& context) {
    // Left and right are either the extents of a part-select, in which case they must
    // both be constant, or the left hand side is the start and the right hand side is
    // the width of an indexed part select, in which case only the rhs need be constant.
    RangeSelectionKind selectionKind;
    switch (syntax.kind) {
        case SyntaxKind::SimpleRangeSelect:
            selectionKind = RangeSelectionKind::Simple;
            break;
        case SyntaxKind::AscendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedUp;
            break;
        case SyntaxKind::DescendingRangeSelect:
            selectionKind = RangeSelectionKind::IndexedDown;
            break;
        default:
            THROW_UNREACHABLE;
    }

    if (!value.bad() && value.type->isAssociativeArray()) {
        context.addDiag(diag::RangeSelectAssociative, fullRange);
        return badExpr(compilation, nullptr);
    }

    // Selection expressions don't need to be const if we're selecting from a queue.
    bool isQueue = value.type->isQueue();
    bool leftConst = !isQueue && selectionKind == RangeSelectionKind::Simple;
    bool rightConst = !isQueue;

    const Expression& left = leftConst ? bind(*syntax.left, context, BindFlags::Constant)
                                       : selfDetermined(compilation, *syntax.left, context);
    const Expression& right = rightConst ? bind(*syntax.right, context, BindFlags::Constant)
                                         : selfDetermined(compilation, *syntax.right, context);

    auto result = compilation.emplace<RangeSelectExpression>(
        selectionKind, compilation.getErrorType(), value, left, right, fullRange);

    if (value.bad() || left.bad() || right.bad())
        return badExpr(compilation, result);

    if (!left.type->isIntegral()) {
        context.addDiag(diag::ExprMustBeIntegral, left.sourceRange);
        return badExpr(compilation, result);
    }
    if (!right.type->isIntegral()) {
        context.addDiag(diag::ExprMustBeIntegral, right.sourceRange);
        return badExpr(compilation, result);
    }

    const Type& valueType = *value.type;
    const Type& elementType = getIndexedType(compilation, context, valueType, syntax.sourceRange(),
                                             value.sourceRange, true);
    if (elementType.isError())
        return badExpr(compilation, result);

    // If this is selecting from a queue, the result is always a queue.
    if (isQueue) {
        result->type = compilation.emplace<QueueType>(elementType, 0u);
        return *result;
    }

    // If not a queue, rhs must always be a constant integer.
    optional<int32_t> rv = context.evalInteger(right);
    if (!rv)
        return badExpr(compilation, result);

    // If the array type has a fixed range, validate that the range we're selecting is allowed.
    SourceRange errorRange{ left.sourceRange.start(), right.sourceRange.end() };
    if (valueType.hasFixedRange()) {
        ConstantRange selectionRange;
        ConstantRange valueRange = valueType.getFixedRange();

        // Helper function for validating the bounds of the selection.
        auto validateRange = [&](auto range) {
            if (!valueRange.containsPoint(range.left) || !valueRange.containsPoint(range.right)) {
                auto& diag = context.addDiag(diag::BadRangeExpression, errorRange);
                diag << range.left << range.right;
                diag << valueType;
                return false;
            }
            return true;
        };

        if (selectionKind == RangeSelectionKind::Simple) {
            optional<int32_t> lv = context.evalInteger(left);
            if (!lv)
                return badExpr(compilation, result);

            selectionRange = { *lv, *rv };
            if (selectionRange.isLittleEndian() != valueRange.isLittleEndian() &&
                selectionRange.width() > 1) {
                auto& diag = context.addDiag(diag::SelectEndianMismatch, errorRange);
                diag << valueType;
                return badExpr(compilation, result);
            }

            if (!context.inUnevaluatedBranch() && !validateRange(selectionRange))
                return badExpr(compilation, result);
        }
        else {
            if (!context.requireGtZero(rv, right.sourceRange))
                return badExpr(compilation, result);

            if (bitwidth_t(*rv) > valueRange.width()) {
                auto& diag = context.addDiag(diag::RangeWidthTooLarge, right.sourceRange);
                diag << *rv;
                diag << valueType;
                return badExpr(compilation, result);
            }

            // If the lhs is a known constant, we can check that now too.
            ConstantValue leftVal;
            if (!context.inUnevaluatedBranch() && (leftVal = context.tryEval(left))) {
                optional<int32_t> index = leftVal.integer().as<int32_t>();
                if (!index) {
                    auto& diag = context.addDiag(diag::IndexValueInvalid, left.sourceRange);
                    diag << leftVal;
                    diag << valueType;
                    return badExpr(compilation, result);
                }

                selectionRange =
                    getIndexedRange(selectionKind, *index, *rv, valueRange.isLittleEndian());

                if (!validateRange(selectionRange))
                    return badExpr(compilation, result);
            }
            else {
                // Otherwise, the resulting range will start with the fixed lower bound of the type.
                int32_t count = *rv - 1;
                if (selectionKind == RangeSelectionKind::IndexedUp) {
                    selectionRange.left = valueRange.lower() + count;
                    selectionRange.right = valueRange.lower();
                }
                else {
                    selectionRange.left = valueRange.upper();
                    selectionRange.right = valueRange.upper() - count;
                }

                if (!valueRange.isLittleEndian())
                    selectionRange = selectionRange.reverse();
            }
        }

        // At this point, all expressions are good, ranges have been validated and
        // we know the final width of the selection, so pick the result type and we're done.
        if (valueType.isUnpackedArray()) {
            result->type =
                compilation.emplace<FixedSizeUnpackedArrayType>(elementType, selectionRange);
        }
        else {
            result->type = compilation.emplace<PackedArrayType>(elementType, selectionRange);
        }
    }
    else {
        // Otherwise, this is a dynamic array so we can't validate much. We should check that
        // the selection endianness is correct for simple ranges -- dynamic arrays only
        // permit big endian [0..N] ordering.
        ConstantRange selectionRange;
        if (selectionKind == RangeSelectionKind::Simple) {
            optional<int32_t> lv = context.evalInteger(left);
            if (!lv)
                return badExpr(compilation, result);

            selectionRange = { *lv, *rv };
            if (selectionRange.isLittleEndian() && selectionRange.width() > 1) {
                auto& diag = context.addDiag(diag::SelectEndianDynamic, errorRange);
                diag << valueType;
                return badExpr(compilation, result);
            }
        }
        else {
            if (!context.requireGtZero(rv, right.sourceRange))
                return badExpr(compilation, result);

            selectionRange.left = 0;
            selectionRange.right = *rv - 1;
        }

        result->type = compilation.emplace<FixedSizeUnpackedArrayType>(elementType, selectionRange);
    }

    return *result;
}

Expression& RangeSelectExpression::fromConstant(Compilation& compilation, Expression& value,
                                                ConstantRange range, const BindContext& context) {
    Expression* left = &IntegerLiteral::fromConstant(compilation, range.left);
    selfDetermined(context, left);

    Expression* right = &IntegerLiteral::fromConstant(compilation, range.right);
    selfDetermined(context, right);

    auto result = compilation.emplace<RangeSelectExpression>(RangeSelectionKind::Simple,
                                                             compilation.getErrorType(), value,
                                                             *left, *right, value.sourceRange);
    if (value.bad() || left->bad() || right->bad())
        return badExpr(compilation, result);

    const Type& valueType = *value.type;
    const Type& elementType =
        getIndexedType(compilation, context, valueType, value.sourceRange, value.sourceRange, true);

    if (elementType.isError())
        return badExpr(compilation, result);

    // This method is only called on expressions with a fixed range type.
    ConstantRange valueRange = valueType.getFixedRange();
    ASSERT(range.isLittleEndian() == valueRange.isLittleEndian());
    ASSERT(valueType.hasFixedRange());

    if (valueType.isUnpackedArray())
        result->type = compilation.emplace<FixedSizeUnpackedArrayType>(elementType, range);
    else
        result->type = compilation.emplace<PackedArrayType>(elementType, range);

    return *result;
}

ConstantValue RangeSelectExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    ConstantValue cl = left().eval(context);
    ConstantValue cr = right().eval(context);
    if (!cv || !cl || !cr)
        return nullptr;

    if (value().type->hasFixedRange()) {
        optional<ConstantRange> range = getFixedRange(context, cl, cr);
        if (!range)
            return nullptr;

        return cv.getSlice(range->upper(), range->lower(), nullptr);
    }
    else {
        optional<ConstantRange> range = getDynamicRange(context, cl, cr, cv);
        if (!range)
            return nullptr;

        // If this is a queue, we didn't verify the endianness of the selection.
        // Check if it's reversed here and issue a warning if so.
        if (value().type->isQueue() && range->isLittleEndian() && range->left != range->right) {
            context.addDiag(diag::ConstEvalQueueRange, sourceRange) << range->left << range->right;
            return value().type->getDefaultValue();
        }

        return cv.getSlice(range->upper(), range->lower(),
                           type->getArrayElementType()->getDefaultValue());
    }
}

LValue RangeSelectExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    ConstantValue cl = left().eval(context);
    ConstantValue cr = right().eval(context);
    if (!lval || !cl || !cr)
        return nullptr;

    if (value().type->hasFixedRange()) {
        optional<ConstantRange> range = getFixedRange(context, cl, cr);
        if (!range)
            return nullptr;

        if (value().type->isIntegral())
            lval.addBitSlice(*range);
        else
            lval.addArraySlice(*range, nullptr);
    }
    else {
        optional<ConstantRange> range = getDynamicRange(context, cl, cr, lval.load());
        if (!range)
            return nullptr;

        lval.addArraySlice(*range, type->getArrayElementType()->getDefaultValue());
    }

    return lval;
}

bool RangeSelectExpression::verifyConstantImpl(EvalContext& context) const {
    return value().verifyConstant(context) && left().verifyConstant(context) &&
           right().verifyConstant(context);
}

optional<ConstantRange> RangeSelectExpression::getFixedRange(EvalContext& context,
                                                             const ConstantValue& cl,
                                                             const ConstantValue& cr) const {
    ConstantRange result;
    const Type& valueType = *value().type;
    ConstantRange valueRange = valueType.getFixedRange();

    if (selectionKind == RangeSelectionKind::Simple) {
        result = type->getFixedRange();
    }
    else {
        optional<int32_t> l = cl.integer().as<int32_t>();
        if (!l) {
            context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cl << valueType;
            return std::nullopt;
        }

        optional<int32_t> r = cr.integer().as<int32_t>();
        result = getIndexedRange(selectionKind, *l, *r, valueRange.isLittleEndian());
    }

    if (!valueRange.containsPoint(result.left) || !valueRange.containsPoint(result.right)) {
        auto& diag = context.addDiag(diag::ConstEvalPartSelectInvalid, sourceRange);
        diag << result.left << result.right;
        diag << valueType;
        return std::nullopt;
    }

    if (!valueType.isPackedArray()) {
        if (valueType.isUnpackedArray()) {
            // Unpacked arrays are stored reversed in memory, so reverse the range here.
            valueRange = valueRange.reverse();
        }
        result.left = valueRange.translateIndex(result.left);
        result.right = valueRange.translateIndex(result.right);
        return result;
    }

    // For packed arrays we're potentially selecting multi-bit elements.
    int32_t width = (int32_t)valueType.getArrayElementType()->getBitWidth();
    result.left = valueRange.translateIndex(result.left) * width + width - 1;
    result.right = valueRange.translateIndex(result.right) * width;

    return result;
}

optional<ConstantRange> RangeSelectExpression::getDynamicRange(EvalContext& context,
                                                               const ConstantValue& cl,
                                                               const ConstantValue& cr,
                                                               const ConstantValue& cv) const {
    const Type& valueType = *value().type;
    optional<int32_t> li = cl.integer().as<int32_t>();
    optional<int32_t> ri = cr.integer().as<int32_t>();
    if (!li) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cl << valueType;
        return std::nullopt;
    }
    if (!ri) {
        context.addDiag(diag::ConstEvalArrayIndexInvalid, sourceRange) << cr << valueType;
        return std::nullopt;
    }

    int32_t l = *li;
    int32_t r = *ri;
    ConstantRange result;

    if (selectionKind == RangeSelectionKind::Simple) {
        result = { l, r };
    }
    else {
        int32_t count = r - 1;
        if (selectionKind == RangeSelectionKind::IndexedUp) {
            result.left = l;
            result.right = l + count;
        }
        else {
            result.left = l - count;
            result.right = l;
        }
    }

    // Out of bounds ranges are allowed, we just issue a warning.
    size_t size = cv.size();
    if (l < 0 || r < 0 || size_t(r) >= size) {
        auto& diag = context.addDiag(diag::ConstEvalDynamicArrayRange, sourceRange);
        diag << result.left << result.right;
        diag << valueType;
        diag << size;
    }

    return result;
}

ConstantRange RangeSelectExpression::getIndexedRange(RangeSelectionKind kind, int32_t l, int32_t r,
                                                     bool littleEndian) {
    // TODO: avoid overflow
    ConstantRange result;
    int32_t count = r - 1;
    if (kind == RangeSelectionKind::IndexedUp) {
        result.left = l + count;
        result.right = l;
    }
    else {
        result.left = l;
        result.right = l - count;
    }

    if (!littleEndian)
        return result.reverse();

    return result;
}

void RangeSelectExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("selectionKind", toString(selectionKind));
    serializer.write("value", value());
    serializer.write("left", left());
    serializer.write("right", right());
}

Expression& MemberAccessExpression::fromSelector(Compilation& compilation, Expression& expr,
                                                 const LookupResult::MemberSelector& selector,
                                                 const InvocationExpressionSyntax* invocation,
                                                 const BindContext& context) {
    // This might look like a member access but actually be a built-in type method.
    const Type& type = expr.type->getCanonicalType();
    switch (type.kind) {
        case SymbolKind::PackedStructType:
        case SymbolKind::UnpackedStructType:
        case SymbolKind::PackedUnionType:
        case SymbolKind::UnpackedUnionType:
            break;
        case SymbolKind::EnumType:
        case SymbolKind::StringType:
        case SymbolKind::FixedSizeUnpackedArrayType:
        case SymbolKind::DynamicArrayType:
        case SymbolKind::AssociativeArrayType:
        case SymbolKind::QueueType:
            return CallExpression::fromSystemMethod(compilation, expr, selector, invocation,
                                                    context);
        default: {
            auto& diag = context.addDiag(diag::InvalidMemberAccess, selector.dotLocation);
            diag << expr.sourceRange;
            diag << selector.nameRange;
            diag << *expr.type;
            return badExpr(compilation, &expr);
        }
    }

    const Symbol* member = expr.type->getCanonicalType().as<Scope>().find(selector.name);
    if (!member) {
        auto& diag = context.addDiag(diag::UnknownMember, selector.nameRange.start());
        diag << expr.sourceRange;
        diag << selector.name;
        diag << *expr.type;
        return badExpr(compilation, &expr);
    }

    // The source range of the entire member access starts from the value being selected.
    SourceRange range{ expr.sourceRange.start(), selector.nameRange.end() };
    const auto& field = member->as<FieldSymbol>();
    return *compilation.emplace<MemberAccessExpression>(field.getType(), expr, field, range);
}

Expression& MemberAccessExpression::fromSyntax(Compilation& compilation,
                                               const MemberAccessExpressionSyntax& syntax,
                                               const InvocationExpressionSyntax* invocation,
                                               const BindContext& context) {
    auto name = syntax.name.valueText();
    Expression& lhs = selfDetermined(compilation, *syntax.left, context);
    if (lhs.bad() || name.empty())
        return badExpr(compilation, &lhs);

    LookupResult::MemberSelector selector;
    selector.name = name;
    selector.dotLocation = syntax.dot.location();
    selector.nameRange = syntax.name.range();
    return fromSelector(compilation, lhs, selector, invocation, context);
}

ConstantValue MemberAccessExpression::evalImpl(EvalContext& context) const {
    ConstantValue cv = value().eval(context);
    if (!cv)
        return nullptr;

    // TODO: handle unpacked unions
    if (value().type->isUnpackedStruct())
        return cv.elements()[field.offset];

    int32_t offset = (int32_t)field.offset;
    int32_t width = (int32_t)type->getBitWidth();
    return cv.integer().slice(width + offset - 1, offset);
}

LValue MemberAccessExpression::evalLValueImpl(EvalContext& context) const {
    LValue lval = value().evalLValue(context);
    if (!lval)
        return nullptr;

    // TODO: handle unpacked unions
    int32_t offset = (int32_t)field.offset;
    if (value().type->isUnpackedStruct()) {
        lval.addIndex(offset, nullptr);
    }
    else {
        int32_t width = (int32_t)type->getBitWidth();
        lval.addBitSlice({ width + offset - 1, offset });
    }

    return lval;
}

bool MemberAccessExpression::verifyConstantImpl(EvalContext& context) const {
    return value().verifyConstant(context);
}

void MemberAccessExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.writeLink("field", field);
    serializer.write("value", value());
}

Expression& CallExpression::fromSyntax(Compilation& compilation,
                                       const InvocationExpressionSyntax& syntax,
                                       const BindContext& context) {
    if (syntax.left->kind == SyntaxKind::MemberAccessExpression) {
        return MemberAccessExpression::fromSyntax(
            compilation, syntax.left->as<MemberAccessExpressionSyntax>(), &syntax, context);
    }

    if (!NameSyntax::isKind(syntax.left->kind)) {
        SourceLocation loc = syntax.arguments ? syntax.arguments->openParen.location()
                                              : syntax.left->getFirstToken().location();
        auto& diag = context.addDiag(diag::ExpressionNotCallable, loc);
        diag << syntax.left->sourceRange();
        return badExpr(compilation, nullptr);
    }

    return bindName(compilation, syntax.left->as<NameSyntax>(), &syntax, context);
}

Expression& CallExpression::fromLookup(Compilation& compilation, const Subroutine& subroutine,
                                       const InvocationExpressionSyntax* syntax, SourceRange range,
                                       const BindContext& context) {
    if (subroutine.index() == 1) {
        const SystemCallInfo& info = std::get<1>(subroutine);
        return createSystemCall(compilation, *info.subroutine, nullptr, syntax, range, context);
    }

    // Collect all arguments into a list of ordered expressions (which can
    // optionally be nullptr to indicate an empty argument) and a map of
    // named argument assignments.
    SmallVectorSized<const SyntaxNode*, 8> orderedArgs;
    SmallMap<string_view, std::pair<const NamedArgumentSyntax*, bool>, 8> namedArgs;

    if (syntax && syntax->arguments) {
        for (auto arg : syntax->arguments->parameters) {
            if (arg->kind == SyntaxKind::NamedArgument) {
                const NamedArgumentSyntax& nas = arg->as<NamedArgumentSyntax>();
                auto name = nas.name.valueText();
                if (!name.empty()) {
                    auto pair = namedArgs.emplace(name, std::make_pair(&nas, false));
                    if (!pair.second) {
                        auto& diag =
                            context.addDiag(diag::DuplicateArgAssignment, nas.name.location());
                        diag << name;
                        diag.addNote(diag::NotePreviousUsage,
                                     pair.first->second.first->name.location());
                    }
                }
            }
            else {
                // Once a named argument has been seen, no more ordered arguments are allowed.
                if (!namedArgs.empty()) {
                    context.addDiag(diag::MixingOrderedAndNamedArgs,
                                    arg->getFirstToken().location());
                    return badExpr(compilation, nullptr);
                }

                if (arg->kind == SyntaxKind::EmptyArgument)
                    orderedArgs.append(arg);
                else
                    orderedArgs.append(arg->as<OrderedArgumentSyntax>().expr);
            }
        }
    }

    // Now bind all arguments.
    bool bad = false;
    uint32_t orderedIndex = 0;
    SmallVectorSized<const Expression*, 8> boundArgs;
    const SubroutineSymbol& symbol = *std::get<0>(subroutine);

    for (auto formal : symbol.arguments) {
        const Expression* expr = nullptr;
        if (orderedIndex < orderedArgs.size()) {
            auto arg = orderedArgs[orderedIndex++];
            if (arg->kind == SyntaxKind::EmptyArgument) {
                // Empty arguments are allowed as long as a default is provided.
                expr = formal->getInitializer();
                if (!expr)
                    context.addDiag(diag::ArgCannotBeEmpty, arg->sourceRange()) << formal->name;
            }
            else {
                expr = &Expression::bindArgument(formal->getType(), formal->direction,
                                                 arg->as<ExpressionSyntax>(), context);
            }

            // Make sure there isn't also a named value for this argument.
            if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
                auto& diag = context.addDiag(diag::DuplicateArgAssignment,
                                             it->second.first->name.location());
                diag << formal->name;
                diag.addNote(diag::NotePreviousUsage, arg->getFirstToken().location());
                it->second.second = true;
                bad = true;
            }
        }
        else if (auto it = namedArgs.find(formal->name); it != namedArgs.end()) {
            // Mark this argument as used so that we can later detect if
            // any were unused.
            it->second.second = true;

            auto arg = it->second.first->expr;
            if (!arg) {
                // Empty arguments are allowed as long as a default is provided.
                expr = formal->getInitializer();
                if (!expr) {
                    context.addDiag(diag::ArgCannotBeEmpty, it->second.first->sourceRange())
                        << formal->name;
                }
            }
            else {
                expr =
                    &Expression::bindArgument(formal->getType(), formal->direction, *arg, context);
            }
        }
        else {
            expr = formal->getInitializer();
            if (!expr) {
                if (namedArgs.empty()) {
                    auto& diag = context.addDiag(diag::TooFewArguments, range);
                    diag << symbol.arguments.size() << orderedArgs.size();
                    bad = true;
                    break;
                }
                else {
                    context.addDiag(diag::UnconnectedArg, range) << formal->name;
                }
            }
        }

        if (!expr) {
            bad = true;
        }
        else {
            bad |= expr->bad();
            boundArgs.append(expr);
        }
    }

    // Make sure there weren't too many ordered arguments provided.
    if (orderedIndex < orderedArgs.size()) {
        auto& diag = context.addDiag(diag::TooManyArguments, range);
        diag << symbol.arguments.size();
        diag << orderedArgs.size();
        bad = true;
    }

    for (const auto& pair : namedArgs) {
        // We marked all the args that we used, so anything left over is an arg assignment
        // for a non-existent arg.
        if (!pair.second.second) {
            auto& diag = context.addDiag(diag::ArgDoesNotExist, pair.second.first->name.location());
            diag << pair.second.first->name.valueText();
            diag << symbol.name;
            bad = true;
        }
    }

    auto result = compilation.emplace<CallExpression>(&symbol, symbol.getReturnType(),
                                                      boundArgs.copy(compilation),
                                                      context.lookupLocation, range);
    if (syntax)
        context.setAttributes(*result, syntax->attributes);

    if (bad)
        return badExpr(compilation, result);

    return *result;
}

Expression& CallExpression::fromSystemMethod(Compilation& compilation, const Expression& expr,
                                             const LookupResult::MemberSelector& selector,
                                             const InvocationExpressionSyntax* syntax,
                                             const BindContext& context) {
    const Type& type = expr.type->getCanonicalType();
    auto subroutine = compilation.getSystemMethod(type.kind, selector.name);
    if (!subroutine) {
        if (syntax) {
            context.addDiag(diag::UnknownSystemMethod, selector.nameRange)
                << selector.name << *expr.type;
        }
        else {
            auto& diag = context.addDiag(diag::InvalidMemberAccess, selector.dotLocation);
            diag << expr.sourceRange;
            diag << selector.nameRange;
            diag << *expr.type;
        }
        return badExpr(compilation, &expr);
    }

    return createSystemCall(compilation, *subroutine, &expr, syntax,
                            syntax ? syntax->sourceRange() : expr.sourceRange, context);
}

Expression& CallExpression::createSystemCall(Compilation& compilation,
                                             const SystemSubroutine& subroutine,
                                             const Expression* firstArg,
                                             const InvocationExpressionSyntax* syntax,
                                             SourceRange range, const BindContext& context) {
    SmallVectorSized<const Expression*, 8> buffer;
    if (firstArg)
        buffer.append(firstArg);

    if (syntax && syntax->arguments) {
        auto actualArgs = syntax->arguments->parameters;
        for (size_t i = 0; i < actualArgs.size(); i++) {
            size_t index = i + (firstArg ? 1 : 0);
            switch (actualArgs[i]->kind) {
                case SyntaxKind::OrderedArgument: {
                    const auto& arg = actualArgs[i]->as<OrderedArgumentSyntax>();
                    buffer.append(&subroutine.bindArgument(index, context, *arg.expr, buffer));
                    break;
                }
                case SyntaxKind::NamedArgument:
                    context.addDiag(diag::NamedArgNotAllowed, actualArgs[i]->sourceRange());
                    return badExpr(compilation, nullptr);
                case SyntaxKind::EmptyArgument:
                    if (subroutine.allowEmptyArgument(index)) {
                        buffer.append(compilation.emplace<EmptyArgumentExpression>(
                            compilation.getVoidType(), actualArgs[i]->sourceRange()));
                    }
                    else {
                        context.addDiag(diag::EmptyArgNotAllowed, actualArgs[i]->sourceRange());
                        return badExpr(compilation, nullptr);
                    }
                    break;
                default:
                    THROW_UNREACHABLE;
            }
        }
    }

    SystemCallInfo callInfo{ &subroutine, &context.scope };
    const Type& type = subroutine.checkArguments(context, buffer, range);
    auto expr = compilation.emplace<CallExpression>(callInfo, type, buffer.copy(compilation),
                                                    context.lookupLocation, range);

    if (type.isError())
        return badExpr(compilation, expr);

    for (auto arg : expr->arguments()) {
        if (arg->bad())
            return badExpr(compilation, expr);
    }

    if (syntax)
        context.setAttributes(*expr, syntax->attributes);

    return *expr;
}

ConstantValue CallExpression::evalImpl(EvalContext& context) const {
    // Delegate system calls to their appropriate handler.
    if (isSystemCall()) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->eval(*callInfo.scope, context, arguments());
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    if (!checkConstant(context, symbol, sourceRange))
        return nullptr;

    // Evaluate all argument in the current stack frame.
    SmallVectorSized<ConstantValue, 8> args;
    for (auto arg : arguments()) {
        ConstantValue v = arg->eval(context);
        if (!v)
            return nullptr;
        args.emplace(std::move(v));
    }

    // Push a new stack frame, push argument values as locals.
    if (!context.pushFrame(symbol, sourceRange.start(), lookupLocation))
        return nullptr;

    span<const FormalArgumentSymbol* const> formals = symbol.arguments;
    for (size_t i = 0; i < formals.size(); i++)
        context.createLocal(formals[i], args[i]);

    context.createLocal(symbol.returnValVar);

    using ER = Statement::EvalResult;
    ER er = symbol.getBody(&context).eval(context);

    // If we got a disable result, it means a disable statement was evaluated that
    // targeted a block that wasn't executing. This isn't allowed in a constant expression.
    // Handle this before popping the frame so that we get the stack reported.
    if (er == ER::Disable)
        context.addDiag(diag::ConstEvalDisableTarget, context.getDisableRange());

    ConstantValue result = std::move(*context.findLocal(symbol.returnValVar));
    context.popFrame();

    if (er == ER::Fail || er == ER::Disable)
        return nullptr;

    ASSERT(er == ER::Success || er == ER::Return);
    return result;
}

bool CallExpression::verifyConstantImpl(EvalContext& context) const {
    for (auto arg : arguments()) {
        if (!arg->verifyConstant(context))
            return false;
    }

    if (isSystemCall()) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->verifyConstant(context, arguments(), sourceRange);
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    if (!checkConstant(context, symbol, sourceRange))
        return false;

    if (!context.pushFrame(symbol, sourceRange.start(), lookupLocation))
        return false;

    bool result = symbol.getBody(&context).verifyConstant(context);
    context.popFrame();
    return result;
}

bool CallExpression::checkConstant(EvalContext& context, const SubroutineSymbol& subroutine,
                                   SourceRange range) {
    if (context.isScriptEval())
        return true;

    if (subroutine.subroutineKind == SubroutineKind::Task) {
        context.addDiag(diag::ConstEvalTaskNotConstant, range);
        return false;
    }

    if (subroutine.getReturnType().isVoid()) {
        context.addDiag(diag::ConstEvalVoidNotConstant, range);
        return false;
    }

    for (auto arg : subroutine.arguments) {
        if (arg->direction != ArgumentDirection::In) {
            context.addDiag(diag::ConstEvalFunctionArgDirection, range);
            return false;
        }
    }

    auto scope = subroutine.getParentScope();
    ASSERT(scope);
    if (scope->asSymbol().kind == SymbolKind::GenerateBlock) {
        context.addDiag(diag::ConstEvalFunctionInsideGenerate, range);
        return false;
    }

    return true;
}

string_view CallExpression::getSubroutineName() const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->name;
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    return symbol.name;
}

SubroutineKind CallExpression::getSubroutineKind() const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        return callInfo.subroutine->kind;
    }

    const SubroutineSymbol& symbol = *std::get<0>(subroutine);
    return symbol.subroutineKind;
}

void CallExpression::serializeTo(ASTSerializer& serializer) const {
    if (subroutine.index() == 1) {
        auto& callInfo = std::get<1>(subroutine);
        serializer.write("subroutine", callInfo.subroutine->name);
    }
    else {
        const SubroutineSymbol& symbol = *std::get<0>(subroutine);
        serializer.writeLink("subroutine", symbol);
    }

    if (!arguments().empty()) {
        serializer.startArray("arguments");
        for (auto arg : arguments())
            serializer.serialize(*arg);
        serializer.endArray();
    }
}

Expression& DataTypeExpression::fromSyntax(Compilation& compilation, const DataTypeSyntax& syntax,
                                           const BindContext& context) {
    if ((context.flags & BindFlags::AllowDataType) == 0) {
        context.addDiag(diag::ExpectedExpression, syntax.sourceRange());
        return badExpr(compilation, nullptr);
    }

    const Type& type = compilation.getType(syntax, context.lookupLocation, context.scope);
    return *compilation.emplace<DataTypeExpression>(type, syntax.sourceRange());
}

Expression& HierarchicalReferenceExpression::fromSyntax(Compilation& compilation,
                                                        const NameSyntax& syntax,
                                                        const BindContext& context) {
    LookupResult result;
    Lookup::name(context.scope, syntax, context.lookupLocation, LookupFlags::AllowDeclaredAfter,
                 result);

    if (result.hasError())
        compilation.addDiagnostics(result.getDiagnostics());

    const Symbol* symbol = result.found;
    if (!symbol)
        return badExpr(compilation, nullptr);

    return *compilation.emplace<HierarchicalReferenceExpression>(*symbol, compilation.getVoidType(),
                                                                 syntax.sourceRange());
}

void HierarchicalReferenceExpression::serializeTo(ASTSerializer& serializer) const {
    if (symbol)
        serializer.writeLink("symbol", *symbol);
}

Expression& MinTypMaxExpression::fromSyntax(Compilation& compilation,
                                            const MinTypMaxExpressionSyntax& syntax,
                                            const BindContext& context,
                                            const Type* assignmentTarget) {
    // Only one of the expressions will be considered evaluated.
    auto minFlags = BindFlags::UnevaluatedBranch;
    auto typFlags = BindFlags::UnevaluatedBranch;
    auto maxFlags = BindFlags::UnevaluatedBranch;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            minFlags = BindFlags::None;
            break;
        case MinTypMax::Typ:
            typFlags = BindFlags::None;
            break;
        case MinTypMax::Max:
            maxFlags = BindFlags::None;
            break;
    }

    auto& min = create(compilation, *syntax.min, context, minFlags, assignmentTarget);
    auto& typ = create(compilation, *syntax.typ, context, typFlags, assignmentTarget);
    auto& max = create(compilation, *syntax.max, context, maxFlags, assignmentTarget);

    Expression* selected = nullptr;
    switch (compilation.getOptions().minTypMax) {
        case MinTypMax::Min:
            selected = &min;
            break;
        case MinTypMax::Typ:
            selected = &typ;
            break;
        case MinTypMax::Max:
            selected = &max;
            break;
    }

    auto result = compilation.emplace<MinTypMaxExpression>(*selected->type, min, typ, max, selected,
                                                           syntax.sourceRange());
    if (min.bad() || typ.bad() || max.bad())
        return badExpr(compilation, result);

    return *result;
}

bool MinTypMaxExpression::propagateType(const BindContext& context, const Type& newType) {
    // Only the selected expression gets a propagated type.
    type = &newType;
    contextDetermined(context, selected_, newType);
    return true;
}

ConstantValue MinTypMaxExpression::evalImpl(EvalContext& context) const {
    return selected().eval(context);
}

bool MinTypMaxExpression::verifyConstantImpl(EvalContext& context) const {
    return selected().verifyConstant(context);
}

void MinTypMaxExpression::serializeTo(ASTSerializer& serializer) const {
    serializer.write("selected", selected());
}

} // namespace slang