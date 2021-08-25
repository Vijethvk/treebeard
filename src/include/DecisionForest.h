#ifndef _DECISIONFOREST_H_
#define _DECISIONFOREST_H_

#include <vector>
#include <string>
#include <sstream>
#include <numeric>
#include "TreeTilingDescriptor.h"

namespace mlir
{
namespace decisionforest
{

enum class ReductionType { kAdd, kVoting };
enum class FeatureType { kNumerical, kCategorical };

template <typename ThresholdType=double, typename ReturnType=double, typename FeatureIndexType=int32_t, typename NodeIndexType=int32_t>
class DecisionTree
{
public:
    static constexpr NodeIndexType INVALID_NODE_INDEX = -1;
    struct Node
    {
        ThresholdType threshold;
        FeatureIndexType featureIndex;
        NodeIndexType parent;
        NodeIndexType leftChild;
        NodeIndexType rightChild;
        FeatureType featureType; // TODO For now assuming everything is numerical

        bool operator==(const Node& that) const
        {
            return threshold==that.threshold && featureIndex==that.featureIndex && parent==that.parent &&
                   leftChild==that.leftChild && rightChild==that.rightChild && featureType==that.featureType;
        }

        bool IsLeaf() const
        {
            return leftChild == INVALID_NODE_INDEX && rightChild == INVALID_NODE_INDEX;
        }
    };
    void SetNumberOfFeatures(size_t numFeatures) { m_numFeatures = numFeatures; }
    void SetTreeScalingFactor(ThresholdType scale) { m_scale = scale; }

    // Create a new node in the current tree
    NodeIndexType NewNode(ThresholdType threshold, FeatureIndexType featureIndex)
    { 
        Node node{threshold, featureIndex, INVALID_NODE_INDEX, INVALID_NODE_INDEX, INVALID_NODE_INDEX, FeatureType::kNumerical};
        m_nodes.push_back(node);
        return m_nodes.size() - 1;
    }
    // Set the parent of a node
    void SetNodeParent(NodeIndexType node, NodeIndexType parent) { m_nodes[node].parent = parent; }
    // Set right child of a node
    void SetNodeRightChild(NodeIndexType node, NodeIndexType child) { m_nodes[node].rightChild = child; }
    // Set left child of a node
    void SetNodeLeftChild(NodeIndexType node, NodeIndexType child) { m_nodes[node].leftChild = child; }

    std::string Serialize() const;
    std::string PrintToString() const;

    bool operator==(const DecisionTree<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>& that) const
    {
        return m_nodes==that.m_nodes && m_numFeatures==that.m_numFeatures && m_scale==that.m_scale;
    }

    ReturnType PredictTree(std::vector<ThresholdType>& data) const;
    TreeTilingDescriptor& TilingDescriptor() { return m_tilingDescriptor; }
    const TreeTilingDescriptor& TilingDescriptor() const { return m_tilingDescriptor; }
private:
    std::vector<Node> m_nodes;
    size_t m_numFeatures;
    ThresholdType m_scale;
    TreeTilingDescriptor m_tilingDescriptor;
};

template <typename ThresholdType=double, typename ReturnType=double, typename FeatureIndexType=int32_t, typename NodeIndexType=int32_t>
class DecisionForest
{
public:
    struct Feature
    {
        std::string name;
        std::string type;
    };

    using DecisionTreeType = DecisionTree<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>;
    void SetReductionType(ReductionType reductionType) { m_reductionType = reductionType; }
    void AddFeature(const std::string& featureName, const std::string& type)
    {
        Feature f{featureName, type};
        m_features.push_back(f);
    }
    DecisionTreeType& NewTree()
    { 
        m_trees.push_back(DecisionTreeType());
        return m_trees.back();
    }
    void EndTree() { }
    size_t NumTrees() { return m_trees.size(); }
    DecisionTreeType& GetTree(int64_t index) { return m_trees[index]; }
    const std::vector<Feature>& GetFeatures() const { return m_features; }
    std::string Serialize() const;
    std::string PrintToString() const;
    ReturnType Predict(std::vector<ThresholdType>& data) const;
    bool operator==(const DecisionForest<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>& that) const
    {
        return m_reductionType==that.m_reductionType && m_trees==that.m_trees;
    }
private:
    std::vector<Feature> m_features;
    std::vector<DecisionTreeType> m_trees;
    ReductionType m_reductionType;
};

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
std::string DecisionTree<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::Serialize() const
{
    std::stringstream strStream;
    strStream << m_numFeatures << m_scale;
    for (auto& node : m_nodes)
    {
        strStream << node.threshold;
        strStream << node.featureIndex;
        strStream << node.parent;
        strStream << node.leftChild;
        strStream << node.rightChild;
        strStream << (int32_t)node.featureType; 
    }
    return strStream.str();
}

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
std::string DecisionTree<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::PrintToString() const
{
    std::stringstream strStream;
    strStream << "NumberOfFeatures = " << m_numFeatures << ", Scale = " << m_scale << ", NumberOfNodes = " << m_nodes.size();
    return strStream.str();
}

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
ReturnType DecisionTree<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::PredictTree(std::vector<ThresholdType>& data) const
{
    // go over the features
    assert(m_nodes.size() > 0);
    const Node* node = &m_nodes[0]; // root node
    while (!node->IsLeaf())
    {
      if (node->threshold < data[node->featureIndex])
        node = &m_nodes[node->leftChild];
      else
        node = &m_nodes[node->rightChild];
    }    
    return node->threshold;
}

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
std::string DecisionForest<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::Serialize() const
{
    std::stringstream strStream;
    strStream << (int32_t)m_reductionType << m_trees.size();
    for (auto& tree : m_trees)
        strStream << tree.Serialize();
    return strStream.str();
}

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
std::string DecisionForest<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::PrintToString() const
{
    std::stringstream strStream;
    strStream << "ReductionType = " << (int32_t)m_reductionType << ", #Trees = " << m_trees.size();
    return strStream.str();
}

template <typename ThresholdType, typename ReturnType, typename FeatureIndexType, typename NodeIndexType>
ReturnType DecisionForest<ThresholdType, ReturnType, FeatureIndexType, NodeIndexType>::Predict(std::vector<ThresholdType>& data) const
{
    std::vector<ReturnType> predictions;
    for (auto& tree: m_trees)
        predictions.push_back(tree.PredictTree(data));
    
    assert(m_reductionType == ReductionType::kAdd);
    return std::accumulate(predictions.begin(), predictions.end(), 0.0);
}

} // decisionforest
} // mlir
#endif // _DECISIONFOREST_H_