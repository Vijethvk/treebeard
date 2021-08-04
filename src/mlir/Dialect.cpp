#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Transforms/InliningUtils.h"
#include "Dialect.h"

using namespace mlir;
using namespace mlir::decisionforest;

#include "Dialect.cpp.inc"

// Initialize the dialect
void DecisionForestDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "Ops.cpp.inc"
      >();
  addTypes<TreeEnsembleType, TreeType>();
  addAttributes<DecisionTreeAttr>();
}

/// Parse a type registered to this dialect.
::mlir::Type DecisionForestDialect::parseType(::mlir::DialectAsmParser &parser) const
{
    return ::mlir::Type();
}

/// Print a type registered to this dialect.
void DecisionForestDialect::printType(::mlir::Type type,
                                      ::mlir::DialectAsmPrinter &os) const
{

}

#define GET_OP_CLASSES
#include "Ops.cpp.inc"