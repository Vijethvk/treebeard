#ifndef _TREEATTRIBUTES_H_
#define _TREEATTRIBUTES_H_

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Dialect.h"
#include "mlir/Interfaces/CastInterfaces.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#include "DecisionForest.h"
#include "DecisionTreeTypes.h"

namespace mlir 
{
namespace decisionforest
{
namespace detail
{
struct DecisionTreeAttrStorage : public ::mlir::AttributeStorage
{
    DecisionTreeAttrStorage(::mlir::Type type, const DecisionForest<>& forest, int64_t index)
      : ::mlir::AttributeStorage(type), m_forest(forest), m_index(index) { }

    /// The hash key is a tuple of the parameter types.
    using KeyTy = std::tuple<::mlir::Type, DecisionForest<>, int64_t>;

    bool operator==(const KeyTy &tblgenKey) const {
        if (!(getType() == std::get<0>(tblgenKey)))
            return false;
        if (!(m_forest == std::get<1>(tblgenKey)))
            return false;
        if (!(m_index == std::get<2>(tblgenKey)))
            return false;
        return true;
    }
    
    static ::llvm::hash_code hashKey(const KeyTy &tblgenKey) {
        auto& forest = std::get<1>(tblgenKey);
        auto index = std::get<2>(tblgenKey);
        return ::llvm::hash_combine(std::get<0>(tblgenKey), forest.Serialize(), std::to_string(index));
    }

    /// Define a construction method for creating a new instance of this
    /// storage.
    static DecisionTreeAttrStorage *construct(::mlir::AttributeStorageAllocator &allocator,
                          const KeyTy &tblgenKey) {
        auto type = std::get<0>(tblgenKey);
        auto& forest = std::get<1>(tblgenKey);
        auto index = std::get<2>(tblgenKey);
        return new (allocator.allocate<DecisionTreeAttrStorage>())
                    DecisionTreeAttrStorage(type, forest, index);
    }
    DecisionForest<> m_forest;
    int64_t m_index;
};

// TODO How do we use templatization of DecisionForest here?
struct DecisionForestAttrStorage : public ::mlir::AttributeStorage
{
    DecisionForestAttrStorage(::mlir::Type type, const DecisionForest<>& forest)
      : ::mlir::AttributeStorage(type), m_forest(forest) { }

    /// The hash key is a tuple of the parameter types.
    using KeyTy = std::tuple<::mlir::Type, DecisionForest<>>;

    bool operator==(const KeyTy &tblgenKey) const {
        if (!(getType() == std::get<0>(tblgenKey)))
            return false;
        if (!(m_forest == std::get<1>(tblgenKey)))
            return false;
        return true;
    }
    
    static ::llvm::hash_code hashKey(const KeyTy &tblgenKey) {
        auto& forest = std::get<1>(tblgenKey);
        return ::llvm::hash_combine(std::get<0>(tblgenKey), forest.Serialize());
    }

    /// Define a construction method for creating a new instance of this
    /// storage.
    static DecisionForestAttrStorage *construct(::mlir::AttributeStorageAllocator &allocator,
                          const KeyTy &tblgenKey) {
      auto type = std::get<0>(tblgenKey);
      auto forest = std::get<1>(tblgenKey);

      return new (allocator.allocate<DecisionForestAttrStorage>())
          DecisionForestAttrStorage(type, forest);
    }
    DecisionForest<> m_forest;
};

struct PredictionOffsetAttrStorage : public ::mlir::AttributeStorage
{
    PredictionOffsetAttrStorage(::mlir::Type type, double_t predictionOffset)
      : ::mlir::AttributeStorage(type), m_predictionOffset(predictionOffset) { }

    /// The hash key is a tuple of the parameter types.
    using KeyTy = std::tuple<::mlir::Type, double_t>;

    bool operator==(const KeyTy &tblgenKey) const {
        if (getType() != std::get<0>(tblgenKey))
            return false;

        if (m_predictionOffset != std::get<1>(tblgenKey))
            return false;

        return true;
    }
    
    static ::llvm::hash_code hashKey(const KeyTy &tblgenKey) {
        auto predictionOffset = std::get<1>(tblgenKey);
        return ::llvm::hash_combine(std::get<0>(tblgenKey), std::to_string(predictionOffset));
    }

    /// Define a construction method for creating a new instance of this
    /// storage.
    static PredictionOffsetAttrStorage *construct(::mlir::AttributeStorageAllocator &allocator,
                          const KeyTy &tblgenKey) {
        auto type = std::get<0>(tblgenKey);
        auto predictionOffset = std::get<1>(tblgenKey);
        return new (allocator.allocate<PredictionOffsetAttrStorage>()) PredictionOffsetAttrStorage(type, predictionOffset);
    }

    double_t m_predictionOffset;
};

} // namespace detail

class DecisionTreeAttribute : public ::mlir::Attribute::AttrBase<DecisionTreeAttribute, ::mlir::Attribute,
                                         detail::DecisionTreeAttrStorage>
{
public:
    /// Inherit some necessary constructors from 'AttrBase'.
    using Base::Base;
    // using ValueType = APInt;
    static DecisionTreeAttribute get(Type type,  DecisionForest<>& forest, int64_t index) {
        return Base::get(type.getContext(), type, forest, index);
    }
    std::string Serialize() {
        int64_t index = getImpl()->m_index;
        return getImpl()->m_forest.GetTree(index).Serialize();
    }
    void Print(::mlir::DialectAsmPrinter &os) {
        int64_t index = getImpl()->m_index;
        auto& tree = getImpl()->m_forest.GetTree(index);
        std::string treeStr = tree.PrintToString();
        os << "Tree = ( " << treeStr << ") treeType = (" << getImpl()->getType() << ")";
    }

};

class DecisionForestAttribute : public ::mlir::Attribute::AttrBase<DecisionForestAttribute, ::mlir::Attribute,
                                                                   detail::DecisionForestAttrStorage>
{
public:
    /// Inherit some necessary constructors from 'AttrBase'.
    using Base::Base;

    static DecisionForestAttribute get(Type type, DecisionForest<>& value) {
        return Base::get(type.getContext(), type, value);
    }
    std::string Serialize() {
        return getImpl()->m_forest.Serialize();
    }
    void Print(::mlir::DialectAsmPrinter &os) {
        std::string forestStr = getImpl()->m_forest.PrintToString();
        auto ensembleMLIRType = getImpl()->getType();
        mlir::decisionforest::TreeEnsembleType ensembleType = ensembleMLIRType.cast<mlir::decisionforest::TreeEnsembleType>();
        os << "Forest = ( " << forestStr << " ) forestType = (" << ensembleType << ")";
    }
    DecisionForest<>& GetDecisionForest() {
        return getImpl()->m_forest;
    }
};

class PredictionOffsetAttribute : public ::mlir::Attribute::AttrBase<PredictionOffsetAttribute, ::mlir::Attribute,
                                                                   detail::PredictionOffsetAttrStorage>
{
public:
    /// Inherit some necessary constructors from 'AttrBase'.
    using Base::Base;

    static PredictionOffsetAttribute get(Type type, double_t offset) {
        return Base::get(type.getContext(), type, offset);
    }
    std::string Serialize() {
        return std::to_string(getImpl()->m_predictionOffset);
    }
    void Print(::mlir::DialectAsmPrinter &os) {
        std::string offsetStr = std::to_string(getImpl()->m_predictionOffset);
        os << "PredictionOffset = ( " << offsetStr << " ) ";
    }

    double_t GetPredictionOffset() {
        return getImpl()->m_predictionOffset;
    }
};

}
}
#endif // _TREEATTRIBUTES_H_