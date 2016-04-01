/*
 * Split.h
 *
 *  Created on: Mar 29, 2016
 *      Author: derek
 */

#ifndef INCLUDE_MORPH_NODE_H_
#define INCLUDE_MORPH_NODE_H_

#include <cassert>
#include <string>
#include <unordered_map>
#include <stack>

#include "morph.h"
#include "types.h"

namespace morfessor
{

class Morph;
class Corpus;
class Model;

// Represents a possible split or a word or morph into two smaller morphs.
class MorphNode
{
public:
  MorphNode(const std::string& morph) noexcept;
  explicit MorphNode(const std::string& morph, size_t count) noexcept;
  explicit MorphNode(const Morph& morph) noexcept;
  bool has_children() const noexcept {
    return left_child_ != nullptr && right_child_ != nullptr;
  }
  std::string morph() const noexcept { return morph_; }
  MorphNode* left_child() const noexcept { return left_child_; }
  MorphNode* right_child() const noexcept { return right_child_; }
  bool operator==(const MorphNode& other) const noexcept {
    return morph_ == other.morph_;
  }

private:
  friend class SegmentationTree;
  // Stores the string/substring this node refers to
  std::string morph_;
	// Left and right children in the binary splitting tree.
	// Will either both be null or both point to a child.
	mutable MorphNode* left_child_;
	mutable MorphNode* right_child_;
};

void resplitnode(MorphNode* node, Model* model, const Corpus& corpus);

} // namespace morfessor

namespace std
{

template <>
struct hash<morfessor::MorphNode>
{
  size_t operator()(const morfessor::MorphNode& k) const noexcept
  {
    return hash<string>()(k.morph());
  }
};

} // namespace std;

namespace morfessor
{

// Stores recursive segmentations of a set of words.
// Example:
//     SegmentationTree segmentations{};
//     segmentations.emplace("reopen", 1);
//     segmentations.split("reopen", 2);
class SegmentationTree
{
 public:
  explicit SegmentationTree() noexcept;
  template <typename InputIterator>
  explicit SegmentationTree(InputIterator first, InputIterator last);
  void split(const std::string& morph, size_t split_index);
  bool contains(const std::string& morph) const {
    return nodes_.find(morph) != nodes_.end();
  }
  void emplace(const std::string& morph, size_t frequency) {
    nodes_.emplace(morph, frequency);
  }
  size_t& at(const std::string& morph) { return nodes_.at(morph); }
  const size_t& at(const std::string& morph) const { return nodes_.at(morph); }
  void Remove(const std::string morph);
 private:
  std::unordered_map<MorphNode, size_t> nodes_;
  Probability pr_model_given_corpus_ = 0;
  Probability pr_corpus_given_model_ = 0;
  Probability pr_frequencies_ = 0;
  Probability pr_lengths_ = 0;
  void ResplitNode(const MorphNode& node);
  void RemoveNode(const MorphNode& node_to_remove, const MorphNode& subtree);
};

template <typename InputIterator>
SegmentationTree::SegmentationTree(InputIterator first, InputIterator last)
{
  while (first != last)
  {
    nodes_.emplace(first->letters(), first->frequency());
    ++first;
  }
}

inline void SegmentationTree::Remove(const std::string morph)
{
  auto node = nodes_.find(morph);
  assert(node != end(nodes_));
  RemoveNode(node->first, node->first);
}

} // namespace morfessor

#endif /* INCLUDE_MORPH_NODE_H_ */
