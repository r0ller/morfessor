// The MIT License (MIT)
//
// Copyright (c) 2016 Derek Felson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "morph_node.h"

#include <cassert>
#include <array>
#include <iostream>
#include <iomanip>
#include <random>
#include <fstream>

#include <boost/math/distributions/gamma.hpp>
#include <boost/math/special_functions/binomial.hpp>

#include "corpus.h"
#include "morph.h"

namespace morfessor {

SegmentationTree::SegmentationTree()
    : nodes_{} {}

void SegmentationTree::Split(const std::string& morph, size_t left_length) {
  assert(morph.size() > 1);
  assert(left_length > 0 && left_length < morph.size());
  auto found_node = nodes_.find(morph);
  assert(found_node != end(nodes_));
  MorphNode* node = &found_node->second;
  assert(!node->has_children());

  node->left_child = morph.substr(0, left_length);
  node->right_child = morph.substr(left_length);
  total_morph_tokens_ -= node->count;  // No longer a leaf node
  IncreaseNodeCount(node->left_child, node->count);
  IncreaseNodeCount(node->right_child, node->count);

  // We lost one unique morph by splitting what we started with, but we
  // may have gained up to two new unique morphs, depending on whether
  // the results of the split were already morphs we knew about.
  unique_morph_types_ += -1
      + static_cast<int>(nodes_[node->left_child].count == node->count)
      + static_cast<int>(nodes_[node->right_child].count == node->count);
}

void SegmentationTree::IncreaseNodeCount(const std::string& subtree_key,
    size_t increase) {
  MorphNode& subtree = nodes_[subtree_key];

  // Recursively update the node's children, if they exist
  if (!subtree.left_child.empty()) {
    IncreaseNodeCount(subtree.left_child, increase);
  }
  if (!subtree.right_child.empty()) {
    IncreaseNodeCount(subtree.right_child, increase);
  }

  subtree.count += increase;

  // Update probabilities if subtree is leaf node
  if (!subtree.has_children()) {
    total_morph_tokens_ += increase;
  }
}

Probability SegmentationTree::ProbabilityOfCorpusGivenModel() const {
  Probability sum = 0;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      sum -= ProbabilityOfMorph(iter.first) * iter.second.count;
    }
  }
  return sum / std::log(2);
}

Probability SegmentationTree::ProbabilityFromImplicitFrequencies() const {
  // Formula without approximation
  if (total_morph_tokens_ < 100) {
    return std::log2(boost::math::binomial_coefficient<Probability>(
        total_morph_tokens_ - 1, unique_morph_types_ - 1));
  } else {
    // Formula with logarithmic approximation to binomial coefficients
    // based on Stirling's approximation
    //
    // return (total_morph_tokens_ - 1) * std::log2(total_morph_tokens_ - 2)
    // - (unique_morph_types - 1) * std::log2(unique_morph_types_ - 2)
    // - (total_morph_tokens_ - unique_morph_types_)
    //  * std::log2(total_morph_tokens_ - unique_morph_types_ - 1);
    //
    // The above should be the correct forumula to use here for a fast
    // approximation, but the Morfessor reference implementation uses
    // a slightly different version, which is used below.
    //
    // Formula from reference implementation
    return (total_morph_tokens_ - 1) * std::log2(total_morph_tokens_ - 2)
          - (unique_morph_types_ - 1) * std::log2(unique_morph_types_ - 2)
          - (total_morph_tokens_ - unique_morph_types_)
            * std::log2(total_morph_tokens_ - unique_morph_types_ - 1);
  }
}

Probability SegmentationTree::ProbabilityFromExplicitFrequencies() const {
  auto exponent = std::log2(1 - hapax_legomena_prior_);
  Probability sum = 0;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      sum -= std::log2(std::pow(iter.second.count, exponent)
                      - std::pow(iter.second.count + 1, exponent));
    }
  }
  return sum;
}

std::unordered_map<char, Probability>
SegmentationTree::LetterProbabilities(bool include_end_of_string) const
{
  // Calculate the probabilities of each letter in the corpus
  std::unordered_map<char, Probability> letter_probabilities;
  size_t total_letters = 0;
  size_t unique_morphs = 0;
  size_t total_morph_tokens = 0;
  Probability end_of_morph_string_probability = 0;

  // Get the frequency of all the letters first
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      auto& morph_string = iter.first;
      auto& node = iter.second;
      ++unique_morphs;
      total_morph_tokens += node.count;
      for (auto c : morph_string)
      {
        total_letters += node.count;
        // letter_probabiltieis actually contains count at this point
        letter_probabilities[c] += node.count;
      }
    }
  }

  // Sanity check
  assert(unique_morphs == unique_morph_types_);
  assert(total_morph_tokens == total_morph_tokens_);

  if (include_end_of_string) {
    // We count the "end of morph" character as a letter
    total_letters += total_morph_tokens;
  }

  // Calculate the actual letter probabilities using maximum likelihood
  auto log_total_letters = std::log2(total_letters);
  for (auto iter : letter_probabilities)
  {
    letter_probabilities[iter.first] =
        log_total_letters - std::log2(letter_probabilities[iter.first]);
  }

  if (include_end_of_string) {
    // The "end of morph string" character can be understood to appear
    // at the end of every string, i.e. total_morph_tokens number of times.
    letter_probabilities['#'] =
        log_total_letters - std::log2(total_morph_tokens);
  }

  return letter_probabilities;
}

Probability SegmentationTree::ProbabilityFromImplicitLengths() const {
  if (letter_probabilities_.size() == 0) {
    letter_probabilities_ = LetterProbabilities(true);
  }
  Probability sum = 0;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      auto& node = iter.second;
      sum += letter_probabilities_.at('#');
    }
  }

  return sum;
}

Probability SegmentationTree::ProbabilityFromExplicitLengths(
    double prior, double beta) const {
  auto alpha = prior / beta + 1;
  auto gd = boost::math::gamma_distribution<double>{alpha, beta};

  Probability sum = 0;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      sum -= std::log2(boost::math::pdf(gd, iter.first.length()));
    }
  }

  return sum;
}

Probability SegmentationTree::MorphStringCost(bool use_implicit_length) const {
  if (letter_probabilities_.size() == 0) {
    letter_probabilities_ = LetterProbabilities(use_implicit_length);
  }
  Probability sum = 0;
  auto p_end = letter_probabilities_.at('#');
  auto p_not_end = 1 - p_end;

  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      auto& morph_string = iter.first;
      auto& node = iter.second;
      for (auto c : morph_string)
      {
        sum += letter_probabilities_.at(c);
      }
    }
  }

  return sum;
}

Probability SegmentationTree::ProbabilityAdjustmentFromLexiconOrdering()
const {
  // Use the first term of Sterling's approximation
  // log n! ~ n * log(n - 1)
  return (unique_morph_types_ * (1 - std::log(unique_morph_types_)))
      / std::log(2);
}

void SegmentationTree::RemoveNode(const MorphNode& node_to_remove,
    const std::string& subtree_key) {
  MorphNode& subtree = nodes_.at(subtree_key);

  // Recursively remove the node's children, if they exist
  if (!subtree.left_child.empty()) {
    RemoveNode(node_to_remove, subtree.left_child);
  }
  if (!subtree.right_child.empty()) {
    RemoveNode(node_to_remove, subtree.right_child);
  }

  // Decrease the node count at the subtree
  auto count_reduction = node_to_remove.count;
  subtree.count -= count_reduction;

  // Decrease probabilities if subtree is leaf node
  if (!subtree.has_children()) {
    total_morph_tokens_ -= count_reduction;
    pr_corpus_given_model_ -= 0;  // TODO: actual logprob
    pr_frequencies_ -= 0;  // TODO: actual logprob
  }
  // If nothing points to the subtree anymore, delete it
  if (subtree.count == 0) {
    if (!subtree.has_children()) {
      unique_morph_types_ -= 1;
      pr_lengths_ -= 0;  // TODO: actual logprob
    }
    nodes_.erase(nodes_.find(subtree_key));
  }
}

Probability SegmentationTree::LexiconCost(AlgorithmModes mode) const {\
  auto sum = ProbabilityAdjustmentFromLexiconOrdering();
  switch (mode) {
  case AlgorithmModes::kBaseline:
    sum += ProbabilityFromImplicitFrequencies();
    sum += ProbabilityFromImplicitLengths();
    sum += MorphStringCost(true);
    break;
  case AlgorithmModes::kBaselineFreq:
    sum += ProbabilityFromExplicitFrequencies();
    sum += ProbabilityFromImplicitLengths();
    sum += MorphStringCost(true);
    break;
  case AlgorithmModes::kBaselineFreqLength:
    sum += ProbabilityFromExplicitFrequencies();
    sum += ProbabilityFromExplicitLengths();
    sum += MorphStringCost(false);
    break;
  case AlgorithmModes::kBaselineLength:
    sum += ProbabilityFromImplicitFrequencies();
    sum += ProbabilityFromExplicitLengths();
    sum += MorphStringCost(false);
    break;
  }
  return sum;
}

Probability SegmentationTree::OverallCost(AlgorithmModes mode) const {
  return LexiconCost(mode) + ProbabilityOfCorpusGivenModel();
}

void SegmentationTree::Optimize() {
  // Some things only need to be computed once.
  letter_probabilities_ = LetterProbabilities(
      algorithm_mode_ == AlgorithmModes::kBaseline
      || algorithm_mode_ == AlgorithmModes::kBaselineFreq);

  std::vector<std::string> keys;
  // Collect all the nodes we will iterate over
  for (const auto& node_pair : nodes_) {
    keys.push_back(node_pair.first);
  }

  // Word list is randomly shuffled on each iteration
  std::random_device rd;
  std::mt19937 g(rd());

  auto old_cost = OverallCost(algorithm_mode_);
  auto new_cost = old_cost;
  do {
    std::shuffle(keys.begin(), keys.end(), g);

    // Try splitting all the nodes
    old_cost = new_cost;
    for (const auto& key : keys) {
      ResplitNode(key);
    }
    new_cost = OverallCost(algorithm_mode_);
    std::cout << *this;
  } while (old_cost - new_cost > convergence_threshold_ * unique_morph_types_);
}

void SegmentationTree::ResplitNode(std::string morph) {
  assert(morph != "");
  auto frequency = nodes_.at(morph).count;

	// Remove the current representation of the node, if we have it
	if (nodes_.find(morph) != end(nodes_)) {
	  RemoveNode(nodes_.at(morph), morph);
	}

	// First, try the node as a morph of its own
	emplace(morph, frequency);

	// Save a copy of this as our current best solution
	pr_model_given_corpus_ = OverallCost(AlgorithmModes::kBaseline);
	size_t best_split_index = 0;

	// Save the unsplit version of the data structure for later
	auto nosplit_pr_model_given_corpus = pr_model_given_corpus_;
	auto nosplit_unique_morph_types = unique_morph_types_;
	auto nosplit_total_morph_tokens = total_morph_tokens_;
	auto nosplit_data_structure = nodes_;

	// Try every split of the node into two substrings
	for (auto split_index = 1; split_index < morph.size(); ++split_index) {
	  Split(morph, split_index);

	  // See if the split improves the cost
	  auto new_overall_cost = OverallCost(algorithm_mode_);
	  if (new_overall_cost < pr_model_given_corpus_) {
	    pr_model_given_corpus_ = new_overall_cost;
	    best_split_index = split_index;
	  }

	  // Undo the hypothetical split we just made
	  nodes_ = nosplit_data_structure;
	  unique_morph_types_ = nosplit_unique_morph_types;
	  total_morph_tokens_ = nosplit_total_morph_tokens;
	}

	// If the model says we should split, then do it and split recursively
	if (best_split_index > 0) {
	  Split(morph, best_split_index);
	  assert(nodes_.at(morph).left_child != "");
	  assert(nodes_.at(morph).right_child != "");
	  ResplitNode(nodes_.at(morph).left_child);
	  ResplitNode(nodes_.at(morph).right_child);
	}
}

std::ostream& SegmentationTree::print(std::ostream& out) const {
  out << "Overall cost: " << std::setiosflags(std::ios::fixed)
      << std::setprecision(5)
      << OverallCost(algorithm_mode_) << std::endl;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      auto& morph_string = iter.first;
      auto& node = iter.second;
      out << node.count << " " << morph_string << std::endl;
    }
  }
  return out;
}

std::ostream& SegmentationTree::print_dot(std::ostream& out) const {
  out << "digraph segmentation_tree {" << std::endl;
  out << "node [shape=record, fontname=\"Arial\"]" << std::endl;
  for (const auto& iter : nodes_) {
    auto& morph_string = iter.first;
    auto& node = iter.second;
    //out << node.count << " " << morph_string << std::endl;
    out << "\"" << morph_string << "\" [label=\"" << morph_string << "| "
        << node.count << "\"]" << std::endl;
    if (node.left_child != "") {
      out << "\"" << morph_string << "\" -> \""
          << node.left_child << "\"" << std::endl;
    }
    if (node.right_child != "") {
      out << "\"" << morph_string << "\" -> \""
          << node.right_child << "\"" << std::endl;
    }
  }
  out << "}" << std::endl;
  return out;
}

std::ostream& SegmentationTree::print_dot_debug() const {
  auto out = std::ofstream("output-debug.dot");
  return print_dot(out);
}

MorphNode::MorphNode()
    : MorphNode(0) {}

MorphNode::MorphNode(size_t count)
    : count{count}, left_child{}, right_child{} {}

} // namespace morfessor
