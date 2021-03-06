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

#include "segmentation.h"

#include <cassert>
#include <iostream>
#include <iomanip>
#include <random>
#include <fstream>
#include <vector>
#include <memory>

#include "corpus.h"
#include "morph.h"

namespace morfessor {

Segmentation::Segmentation(const Corpus& training_corpus,
    std::shared_ptr<Model> model)
    : nodes_{}, model_{model} {
  // The model has already initialized based on the corpus, so here we just
  // need to add the words to the data structure, without considering their
  // cost.
  for (auto iter = training_corpus.cbegin(); iter != training_corpus.cend();
      ++iter) {
    nodes_.emplace(iter->letters(), iter->frequency());
  }
}

std::shared_ptr<std::vector<std::string> >
Segmentation::SegmentTestCorpus(const Corpus& test_corpus) {
  auto segmentations = std::make_shared<std::vector<std::string> >();
  segmentations->reserve(test_corpus.size());

  auto log_token_count =
      std::log(model_->total_morph_tokens());

  for (auto iter = test_corpus.cbegin(); iter != test_corpus.cend(); ++iter) {
    auto word = iter->letters();
    auto word_length = iter->length();
    auto count = iter->frequency();

    double bad_likelihood = (word_length + 1) * log_token_count;
    double pseudo_infinite_cost = (word_length + 1) * bad_likelihood;

    std::vector<double> delta(word_length + 1, 0.0);
    std::vector<double> psi(word_length + 1, 0.0);

    for (auto end_index = 1; end_index <= word_length; ++end_index) {
      double best_delta = pseudo_infinite_cost;
      double best_length = 0;

      for (auto morph_length = 1; morph_length <= end_index; ++morph_length) {
        auto morph = word.substr(end_index - morph_length, morph_length);
        auto morph_cost = 0;
        if (contains(morph)) {
          morph_cost = log_token_count - std::log(at(morph).count);
        } else if (morph_length == 1) {
          // The morph was undefined, and only one letter long. Accept
          // it with a bad likelihood.
          morph_cost = bad_likelihood;
        } else {
          // The morph was undefined. Keep looking elsewhere.
          continue;
        }

        assert(end_index >= morph_length && end_index - morph_length < delta.size());
        double current_delta = delta[end_index - morph_length] + morph_cost;
        if (current_delta < best_delta) {
          best_delta = current_delta;
          best_length = morph_length;
        }
      }  // for each morph_length

      assert(end_index > 0 && end_index < delta.size());
      delta[end_index] = best_delta;
      psi[end_index] = best_length;
    }  // for each end_index

    std::string str = "";
    auto end_index = word_length;
    while (psi[end_index] != 0) {
      assert(end_index > 0 && end_index < psi.size());
      str = word.substr(end_index - psi[end_index], psi[end_index]) + " " + str;
      end_index -= psi[end_index];
    }
    segmentations->push_back(str);
  }  // for each word

  return segmentations;
}

void Segmentation::AdjustMorphCount(std::string morph, int delta) {
  // Precondition check: Morph string cannot be empty.
  assert(!morph.empty());

  // Either find the morph in the data structure, or create it.
  // The count of a created node is 0.
  MorphNode& subtree = nodes_[morph];

  // Precondition check: Never allow node counts to become negative.
  assert(delta >= 0 || -delta <= subtree.count);

  // We'll be changing the data structure, so we save a copy of the information
  // here. Can't trust pointers and references into it once we start adding
  // and removing nodes, since the map might reorganize its data in memory.
  auto old_count = subtree.count;
  auto new_count = subtree.count + delta;
  auto left_child = subtree.left_child;
  auto right_child = subtree.right_child;

  // Sanity check: Splits are always binary, so if we ever see a case where
  // a node has an odd number of children, we've done something wrong.
  assert (left_child.empty() == right_child.empty());

  if (new_count == 0) {
    nodes_.erase(nodes_.find(morph));
  } else {
    subtree.count = new_count;
  }

  // Recursively update the node's children, if they exist. Otherwise we
  // are dealing with a leaf node, and we have to update our costs to account
  // for the new frequencies. Costs are only over calculated based on leaf
  // nodes.
  if (!left_child.empty()) {
    AdjustMorphCount(left_child, delta);
    AdjustMorphCount(right_child, delta);
  } else {
    model_->adjust_morph_token_count(delta);

    // To adjust the probabilities, we subtract the old contribution of the
    // morph and add the contribution of the new count.
    if (old_count > 0) {
      model_->adjust_corpus_cost(-old_count);
      model_->adjust_frequency_cost(-old_count);
    }
    if (new_count > 0) {
      model_->adjust_corpus_cost(new_count);
      model_->adjust_frequency_cost(new_count);
    }

    if (old_count == 0 && new_count > 0) {
      // Adding a morph
      model_->adjust_unique_morph_count(1);
      model_->adjust_length_cost(morph.length());
      model_->adjust_string_cost(morph, true);
    } else if (new_count == 0 && old_count > 0) {
      // Removing a morph
      model_->adjust_unique_morph_count(-1);
      model_->adjust_length_cost(-morph.length());
      model_->adjust_string_cost(morph, false);
    }
  }
}

void Segmentation::ResplitNode(std::string morph) {
  // Precondition check: The morph cannot be the empty string.
  assert(!morph.empty());

  // We'll be deleting the morph next, so remember its count.
  auto frequency = nodes_.at(morph).count;

  // Remove the current representation of the node, if we have it. This
  // means that we recalculate the best split for a morph ever time we
  // encounter it, which is good since the quality of a new split depends on
  // the splits we've chosen so far. This just makes the algorithm a little
  // less dependent on the order in which morphs are evaluated.
  if (nodes_.find(morph) != end(nodes_)) {
    AdjustMorphCount(morph, -frequency);
  }

  // Recalculate the model with the node unsplit.
  AdjustMorphCount(morph, frequency);

  // Save a copy of this as our current best solution.
  auto best_cost = model_->overall_cost();
  size_t best_split_index = 0;

  // The model only cares about leaf nodes, and since we're going to try some
  // hypothetical splits to find out how they affect the cost, we have to
  // pretend the morph that's being split doesn't exist anymore; as far as the
  // model is concerned, it doesn't. We'll add it back later, one way
  // or another.
  AdjustMorphCount(morph, -frequency);

  // Try every split of the node into two substrings
  for (auto split_index = 1; split_index < morph.size(); ++split_index) {
    // Add the child morphs to the model.
    auto left_child = morph.substr(0, split_index);
    auto right_child = morph.substr(split_index);
    AdjustMorphCount(left_child, frequency);
    AdjustMorphCount(right_child, frequency);

    // See if the split improves the cost
    auto new_cost = model_->overall_cost();
    if (new_cost < best_cost) {
      best_cost = new_cost;
      best_split_index = split_index;
    }

    // Undo the hypothetical split we just made
    AdjustMorphCount(left_child, -frequency);
    AdjustMorphCount(right_child, -frequency);
  }

  if (best_split_index > 0) {
    // Readd the parent to the segmentation data structure, but not to the
    // model, since only leaf nodes count towards the model.
    nodes_[morph].count = frequency;
    nodes_[morph].left_child = morph.substr(0, best_split_index);
    nodes_[morph].right_child = morph.substr(best_split_index);

    // If the model says we should split, then do it and split recursively.
    AdjustMorphCount(nodes_[morph].left_child, frequency);
    AdjustMorphCount(nodes_[morph].right_child, frequency);
    ResplitNode(nodes_[morph].left_child);
    ResplitNode(nodes_[morph].right_child);
  } else {
    // Readd the original morph to the data structure and the model as well.
    AdjustMorphCount(morph, frequency);
  }
}

void Segmentation::Optimize() {
  std::vector<std::string> keys;
  // Collect all the nodes we will iterate over
  for (const auto& node_pair : nodes_) {
    keys.push_back(node_pair.first);
  }

  // Word list is randomly shuffled on each iteration
  std::random_device rd;
  std::mt19937 g(rd());

  auto old_cost = model_->overall_cost();
  auto new_cost = old_cost;
  do {
    std::shuffle(keys.begin(), keys.end(), g);

    // Try splitting all the nodes
    old_cost = new_cost;
    for (const auto& key : keys) {
      ResplitNode(key);
    }
    new_cost = model_->overall_cost();
  } while (old_cost - new_cost > model_->convergence_threshold());
}

std::ostream& Segmentation::print(std::ostream& out) const {
  out << "Overall cost: " << std::setiosflags(std::ios::fixed)
      << std::setprecision(5)
      << model_->overall_cost() << std::endl;
  for (const auto& iter : nodes_) {
    if (!iter.second.has_children()) {
      auto& morph_string = iter.first;
      auto& node = iter.second;
      out << node.count << " " << morph_string << std::endl;
    }
  }
  return out;
}

std::ostream& Segmentation::print_dot(std::ostream& out) const {
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

std::ostream& Segmentation::print_dot_debug() const {
  auto out = std::ofstream("output-debug.dot");
  return print_dot(out);
}

std::ostream& Segmentation::print_as_corpus(std::ostream& out) const {
  for (const auto& iter : nodes_) {
    auto& morph_string = iter.first;
    auto& node = iter.second;
    if (!node.has_children()) {
      out << node.count << " " << morph_string << std::endl;
    }
  }
}

} // namespace morfessor
