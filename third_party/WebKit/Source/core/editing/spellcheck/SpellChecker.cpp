/*
 * Copyright (C) 2006, 2007, 2008, 2011 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/editing/spellcheck/SpellChecker.h"

#include "core/clipboard/DataTransferAccessPolicy.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/NodeTraversal.h"
#include "core/editing/EditingUtilities.h"
#include "core/editing/Editor.h"
#include "core/editing/EphemeralRange.h"
#include "core/editing/FrameSelection.h"
#include "core/editing/SelectionTemplate.h"
#include "core/editing/VisiblePosition.h"
#include "core/editing/VisibleUnits.h"
#include "core/editing/iterators/CharacterIterator.h"
#include "core/editing/markers/DocumentMarkerController.h"
#include "core/editing/markers/SpellCheckMarker.h"
#include "core/editing/spellcheck/IdleSpellCheckCallback.h"
#include "core/editing/spellcheck/SpellCheckRequester.h"
#include "core/editing/spellcheck/TextCheckingParagraph.h"
#include "core/frame/LocalFrame.h"
#include "core/html/forms/HTMLInputElement.h"
#include "core/html_names.h"
#include "core/input_type_names.h"
#include "core/layout/LayoutTextControl.h"
#include "core/loader/EmptyClients.h"
#include "core/page/Page.h"
#include "platform/text/TextBreakIterator.h"
#include "public/platform/WebSpellCheckPanelHostClient.h"
#include "public/platform/WebString.h"
#include "public/web/WebTextCheckClient.h"
#include "public/web/WebTextDecorationType.h"

namespace blink {

using namespace HTMLNames;

namespace {

static bool IsWhiteSpaceOrPunctuation(UChar c) {
  return IsSpaceOrNewline(c) || WTF::Unicode::IsPunct(c);
}

bool IsAmbiguousBoundaryCharacter(UChar character) {
  // These are characters that can behave as word boundaries, but can appear
  // within words. If they are just typed, i.e. if they are immediately followed
  // by a caret, we want to delay text checking until the next character has
  // been typed.
  // FIXME: this is required until 6853027 is fixed and text checking can do
  // this for us.
  return character == '\'' || character == kRightSingleQuotationMarkCharacter ||
         character == kHebrewPunctuationGershayimCharacter;
}

}  // namespace

SpellChecker* SpellChecker::Create(LocalFrame& frame) {
  return new SpellChecker(frame);
}

static WebSpellCheckPanelHostClient& GetEmptySpellCheckPanelHostClient() {
  DEFINE_STATIC_LOCAL(EmptySpellCheckPanelHostClient, client, ());
  return client;
}

WebSpellCheckPanelHostClient& SpellChecker::SpellCheckPanelHostClient() const {
  WebSpellCheckPanelHostClient* spell_check_panel_host_client =
      GetFrame().Client()->SpellCheckPanelHostClient();
  if (!spell_check_panel_host_client)
    return GetEmptySpellCheckPanelHostClient();
  return *spell_check_panel_host_client;
}

WebTextCheckClient* SpellChecker::GetTextCheckerClient() const {
  // There is no frame client if the frame is detached.
  if (!GetFrame().Client())
    return nullptr;
  return GetFrame().Client()->GetTextCheckerClient();
}

SpellChecker::SpellChecker(LocalFrame& frame)
    : frame_(&frame),
      spell_check_requester_(SpellCheckRequester::Create(frame)),
      idle_spell_check_callback_(IdleSpellCheckCallback::Create(frame)) {}

bool SpellChecker::IsSpellCheckingEnabled() const {
  if (WebTextCheckClient* client = GetTextCheckerClient())
    return client->IsSpellCheckingEnabled();
  return false;
}

void SpellChecker::IgnoreSpelling() {
  RemoveMarkers(GetFrame()
                    .Selection()
                    .ComputeVisibleSelectionInDOMTree()
                    .ToNormalizedEphemeralRange(),
                DocumentMarker::kSpelling);
}

void SpellChecker::AdvanceToNextMisspelling(bool start_before_selection) {
  DocumentLifecycle::DisallowTransitionScope disallow_transition(
      GetFrame().GetDocument()->Lifecycle());

  // The basic approach is to search in two phases - from the selection end to
  // the end of the doc, and then we wrap and search from the doc start to
  // (approximately) where we started.

  // Start at the end of the selection, search to edge of document. Starting at
  // the selection end makes repeated "check spelling" commands work.
  VisibleSelection selection(
      GetFrame().Selection().ComputeVisibleSelectionInDOMTree());
  Position spelling_search_start, spelling_search_end;
  Range::selectNodeContents(GetFrame().GetDocument(), spelling_search_start,
                            spelling_search_end);

  bool started_with_selection = false;
  if (selection.Start().AnchorNode()) {
    started_with_selection = true;
    if (start_before_selection) {
      VisiblePosition start(selection.VisibleStart());
      // We match AppKit's rule: Start 1 character before the selection.
      VisiblePosition one_before_start = PreviousPositionOf(start);
      spelling_search_start =
          (one_before_start.IsNotNull() ? one_before_start : start)
              .ToParentAnchoredPosition();
    } else {
      spelling_search_start = selection.VisibleEnd().ToParentAnchoredPosition();
    }
  }

  Position position = spelling_search_start;
  if (!IsEditablePosition(position)) {
    // This shouldn't happen in very often because the Spelling menu items
    // aren't enabled unless the selection is editable.  This can happen in Mail
    // for a mix of non-editable and editable content (like Stationary), when
    // spell checking the whole document before sending the message.  In that
    // case the document might not be editable, but there are editable pockets
    // that need to be spell checked.

    if (!GetFrame().GetDocument()->documentElement())
      return;
    position = FirstEditableVisiblePositionAfterPositionInRoot(
                   position, *GetFrame().GetDocument()->documentElement())
                   .DeepEquivalent();
    if (position.IsNull())
      return;

    spelling_search_start = position.ParentAnchoredEquivalent();
    started_with_selection = false;  // won't need to wrap
  }

  // topNode defines the whole range we want to operate on
  ContainerNode* top_node = HighestEditableRoot(position);
  // TODO(yosin): |lastOffsetForEditing()| is wrong here if
  // |editingIgnoresContent(highestEditableRoot())| returns true, e.g. <table>
  spelling_search_end = Position::EditingPositionOf(
      top_node, EditingStrategy::LastOffsetForEditing(top_node));

  // If spellingSearchRange starts in the middle of a word, advance to the
  // next word so we start checking at a word boundary. Going back by one char
  // and then forward by a word does the trick.
  if (started_with_selection) {
    VisiblePosition one_before_start =
        PreviousPositionOf(CreateVisiblePosition(spelling_search_start));
    if (one_before_start.IsNotNull() &&
        RootEditableElementOf(one_before_start.DeepEquivalent()) ==
            RootEditableElementOf(spelling_search_start))
      spelling_search_start =
          EndOfWord(one_before_start).ToParentAnchoredPosition();
    // else we were already at the start of the editable node
  }

  if (spelling_search_start == spelling_search_end)
    return;  // nothing to search in

  // We go to the end of our first range instead of the start of it, just to be
  // sure we don't get foiled by any word boundary problems at the start. It
  // means we might do a tiny bit more searching.
  Node* search_end_node_after_wrap = spelling_search_end.ComputeContainerNode();
  int search_end_offset_after_wrap =
      spelling_search_end.OffsetInContainerNode();

  std::pair<String, int> misspelled_item(String(), 0);
  String& misspelled_word = misspelled_item.first;
  int& misspelling_offset = misspelled_item.second;
  misspelled_item =
      FindFirstMisspelling(spelling_search_start, spelling_search_end);

  // If we did not find a misspelled word, wrap and try again (but don't bother
  // if we started at the beginning of the block rather than at a selection).
  if (started_with_selection && !misspelled_word) {
    spelling_search_start = Position::EditingPositionOf(top_node, 0);
    // going until the end of the very first chunk we tested is far enough
    spelling_search_end = Position::EditingPositionOf(
        search_end_node_after_wrap, search_end_offset_after_wrap);
    misspelled_item =
        FindFirstMisspelling(spelling_search_start, spelling_search_end);
  }

  if (!misspelled_word.IsEmpty()) {
    // We found a misspelling. Select the misspelling, update the spelling
    // panel, and store a marker so we draw the red squiggle later.

    const EphemeralRange misspelling_range = CalculateCharacterSubrange(
        EphemeralRange(spelling_search_start, spelling_search_end),
        misspelling_offset, misspelled_word.length());
    GetFrame().Selection().SetSelectionAndEndTyping(
        SelectionInDOMTree::Builder()
            .SetBaseAndExtent(misspelling_range)
            .Build());
    GetFrame().Selection().RevealSelection();
    SpellCheckPanelHostClient().UpdateSpellingUIWithMisspelledWord(
        misspelled_word);
    GetFrame().GetDocument()->Markers().AddSpellingMarker(misspelling_range);
  }
}

void SpellChecker::ShowSpellingGuessPanel() {
  if (SpellCheckPanelHostClient().IsShowingSpellingUI()) {
    SpellCheckPanelHostClient().ShowSpellingUI(false);
    return;
  }

  AdvanceToNextMisspelling(true);
  SpellCheckPanelHostClient().ShowSpellingUI(true);
}

static void AddMarker(Document* document,
                      const EphemeralRange& checking_range,
                      DocumentMarker::MarkerType type,
                      int location,
                      int length,
                      const Vector<String>& descriptions) {
  DCHECK(type == DocumentMarker::kSpelling || type == DocumentMarker::kGrammar)
      << type;
  DCHECK_GT(length, 0);
  DCHECK_GE(location, 0);
  const EphemeralRange& range_to_mark =
      CalculateCharacterSubrange(checking_range, location, length);
  if (!SpellChecker::IsSpellCheckingEnabledAt(range_to_mark.StartPosition()))
    return;
  if (!SpellChecker::IsSpellCheckingEnabledAt(range_to_mark.EndPosition()))
    return;

  String description;
  for (size_t i = 0; i < descriptions.size(); ++i) {
    if (i != 0)
      description.append('\n');
    description.append(descriptions[i]);
  }

  if (type == DocumentMarker::kSpelling) {
    document->Markers().AddSpellingMarker(range_to_mark, description);
    return;
  }

  DCHECK_EQ(type, DocumentMarker::kGrammar);
  document->Markers().AddGrammarMarker(range_to_mark, description);
}

void SpellChecker::MarkAndReplaceFor(
    SpellCheckRequest* request,
    const Vector<TextCheckingResult>& results) {
  TRACE_EVENT0("blink", "SpellChecker::markAndReplaceFor");
  DCHECK(request);
  if (!GetFrame().Selection().IsAvailable()) {
    // "editing/spelling/spellcheck-async-remove-frame.html" reaches here.
    return;
  }
  if (!request->IsValid())
    return;
  if (request->RootEditableElement()->GetDocument() !=
      GetFrame().Selection().GetDocument()) {
    // we ignore |request| made for another document.
    // "editing/spelling/spellcheck-sequencenum.html" and others reach here.
    return;
  }

  // TODO(editing-dev): The use of updateStyleAndLayoutIgnorePendingStylesheets
  // needs to be audited.  See http://crbug.com/590369 for more details.
  GetFrame().GetDocument()->UpdateStyleAndLayoutIgnorePendingStylesheets();

  DocumentLifecycle::DisallowTransitionScope disallow_transition(
      GetFrame().GetDocument()->Lifecycle());

  EphemeralRange checking_range(request->CheckingRange());

  // Abort marking if the content of the checking change has been modified.
  String current_content =
      PlainText(checking_range, TextIteratorBehavior::Builder()
                                    .SetEmitsObjectReplacementCharacter(true)
                                    .Build());
  if (current_content != request->GetText()) {
    // "editing/spelling/spellcheck-async-mutation.html" reaches here.
    return;
  }

  // Clear the stale markers.
  RemoveMarkers(checking_range, DocumentMarker::MisspellingMarkers());

  if (!results.size())
    return;

  TextCheckingParagraph paragraph(checking_range, checking_range);

  // TODO(crbug.com/230387): The following comment does not match the current
  // behavior and should be rewritten.
  // Expand the range to encompass entire paragraphs, since text checking needs
  // that much context.
  int ambiguous_boundary_offset = -1;

  if (GetFrame().Selection().ComputeVisibleSelectionInDOMTree().IsCaret()) {
    // TODO(crbug.com/230387): The following comment does not match the current
    // behavior and should be rewritten.
    // Attempt to save the caret position so we can restore it later if needed
    const Position& caret_position =
        GetFrame().Selection().ComputeVisibleSelectionInDOMTree().End();
    const Position& paragraph_start = checking_range.StartPosition();
    const int selection_offset =
        paragraph_start < caret_position
            ? TextIterator::RangeLength(paragraph_start, caret_position)
            : 0;
    if (selection_offset > 0 &&
        static_cast<unsigned>(selection_offset) <=
            paragraph.GetText().length() &&
        IsAmbiguousBoundaryCharacter(
            paragraph.TextCharAt(selection_offset - 1))) {
      ambiguous_boundary_offset = selection_offset - 1;
    }
  }

  const int spelling_range_end_offset = paragraph.CheckingEnd();
  for (const TextCheckingResult& result : results) {
    const int result_location = result.location + paragraph.CheckingStart();
    const int result_length = result.length;
    const bool result_ends_at_ambiguous_boundary =
        ambiguous_boundary_offset >= 0 &&
        result_location + result_length == ambiguous_boundary_offset;

    // Only mark misspelling if:
    // 1. Result falls within spellingRange.
    // 2. The word in question doesn't end at an ambiguous boundary. For
    //    instance, we would not mark "wouldn'" as misspelled right after
    //    apostrophe is typed.
    switch (result.decoration) {
      case kTextDecorationTypeSpelling:
        if (result_location < paragraph.CheckingStart() ||
            result_location + result_length > spelling_range_end_offset ||
            result_ends_at_ambiguous_boundary)
          continue;
        AddMarker(GetFrame().GetDocument(), paragraph.CheckingRange(),
                  DocumentMarker::kSpelling, result_location, result_length,
                  result.replacements);
        continue;

      case kTextDecorationTypeGrammar:
        if (!paragraph.CheckingRangeCovers(result_location, result_length))
          continue;
        DCHECK_GT(result_length, 0);
        DCHECK_GE(result_location, 0);
        for (const GrammarDetail& detail : result.details) {
          DCHECK_GT(detail.length, 0);
          DCHECK_GE(detail.location, 0);
          if (!paragraph.CheckingRangeCovers(result_location + detail.location,
                                             detail.length))
            continue;
          AddMarker(GetFrame().GetDocument(), paragraph.CheckingRange(),
                    DocumentMarker::kGrammar, result_location + detail.location,
                    detail.length, result.replacements);
        }
        continue;
    }
    NOTREACHED();
  }
}

void SpellChecker::DidEndEditingOnTextField(Element* e) {
  TRACE_EVENT0("blink", "SpellChecker::didEndEditingOnTextField");

  // Remove markers when deactivating a selection in an <input type="text"/>.
  // Prevent new ones from appearing too.
  TextControlElement* text_control_element = ToTextControlElement(e);
  HTMLElement* inner_editor = text_control_element->InnerEditorElement();
  RemoveSpellingAndGrammarMarkers(*inner_editor);
}

void SpellChecker::RemoveSpellingAndGrammarMarkers(const HTMLElement& element,
                                                   ElementsType elements_type) {
  // TODO(editing-dev): The use of updateStyleAndLayoutIgnorePendingStylesheets
  // needs to be audited.  See http://crbug.com/590369 for more details.
  GetFrame().GetDocument()->UpdateStyleAndLayoutTreeForNode(&element);

  DocumentMarker::MarkerTypes marker_types(DocumentMarker::kSpelling);
  marker_types.Add(DocumentMarker::kGrammar);
  for (Node& node : NodeTraversal::InclusiveDescendantsOf(element)) {
    if (elements_type == ElementsType::kAll || !HasEditableStyle(node)) {
      GetFrame().GetDocument()->Markers().RemoveMarkersForNode(&node,
                                                               marker_types);
    }
  }
}

std::pair<Node*, SpellCheckMarker*>
SpellChecker::GetSpellCheckMarkerUnderSelection() const {
  const VisibleSelection& selection =
      GetFrame().Selection().ComputeVisibleSelectionInDOMTree();
  if (selection.IsNone())
    return {};

  // Caret and range selections always return valid normalized ranges.
  const EphemeralRange& selection_range = FirstEphemeralRangeOf(selection);

  Node* const selection_start_container =
      selection_range.StartPosition().ComputeContainerNode();
  Node* const selection_end_container =
      selection_range.EndPosition().ComputeContainerNode();

  // We don't currently support the case where a misspelling spans multiple
  // nodes. See crbug.com/720065
  if (selection_start_container != selection_end_container)
    return {};

  if (!selection_start_container->IsTextNode())
    return {};

  const unsigned selection_start_offset =
      selection_range.StartPosition().ComputeOffsetInContainerNode();
  const unsigned selection_end_offset =
      selection_range.EndPosition().ComputeOffsetInContainerNode();

  DocumentMarker* const marker =
      GetFrame().GetDocument()->Markers().FirstMarkerIntersectingOffsetRange(
          ToText(*selection_start_container), selection_start_offset,
          selection_end_offset, DocumentMarker::MisspellingMarkers());
  if (!marker)
    return {};

  return std::make_pair(selection_start_container, ToSpellCheckMarker(marker));
}

std::pair<String, String> SpellChecker::SelectMisspellingAsync() {
  const std::pair<Node*, SpellCheckMarker*>& node_and_marker =
      GetSpellCheckMarkerUnderSelection();
  if (!node_and_marker.first)
    return {};

  Node* const marker_node = node_and_marker.first;
  const SpellCheckMarker* const marker = node_and_marker.second;

  const VisibleSelection& selection =
      GetFrame().Selection().ComputeVisibleSelectionInDOMTree();
  // Caret and range selections (one of which we must have since we found a
  // marker) always return valid normalized ranges.
  const EphemeralRange& selection_range =
      selection.ToNormalizedEphemeralRange();

  const EphemeralRange marker_range(
      Position(marker_node, marker->StartOffset()),
      Position(marker_node, marker->EndOffset()));
  const String& marked_text = PlainText(marker_range);
  if (marked_text.StripWhiteSpace(&IsWhiteSpaceOrPunctuation) !=
      PlainText(selection_range).StripWhiteSpace(&IsWhiteSpaceOrPunctuation))
    return {};

  return std::make_pair(marked_text, marker->Description());
}

void SpellChecker::ReplaceMisspelledRange(const String& text) {
  const std::pair<Node*, SpellCheckMarker*>& node_and_marker =
      GetSpellCheckMarkerUnderSelection();
  if (!node_and_marker.first)
    return;

  Node* const container_node = node_and_marker.first;
  const SpellCheckMarker* const marker = node_and_marker.second;

  GetFrame().Selection().SetSelectionAndEndTyping(
      SelectionInDOMTree::Builder()
          .Collapse(Position(container_node, marker->StartOffset()))
          .Extend(Position(container_node, marker->EndOffset()))
          .Build());

  Document& current_document = *GetFrame().GetDocument();

  // TODO(editing-dev): The use of updateStyleAndLayoutIgnorePendingStylesheets
  // needs to be audited.  See http://crbug.com/590369 for more details.
  current_document.UpdateStyleAndLayoutIgnorePendingStylesheets();

  // Dispatch 'beforeinput'.
  Element* const target = FindEventTargetFrom(
      GetFrame(), GetFrame().Selection().ComputeVisibleSelectionInDOMTree());

  DataTransfer* const data_transfer = DataTransfer::Create(
      DataTransfer::DataTransferType::kInsertReplacementText,
      DataTransferAccessPolicy::kReadable, DataObject::CreateFromString(text));

  const bool cancel = DispatchBeforeInputDataTransfer(
                          target, InputEvent::InputType::kInsertReplacementText,
                          data_transfer) != DispatchEventResult::kNotCanceled;

  // 'beforeinput' event handler may destroy target frame.
  if (current_document != GetFrame().GetDocument())
    return;

  // TODO(editing-dev): The use of updateStyleAndLayoutIgnorePendingStylesheets
  // needs to be audited.  See http://crbug.com/590369 for more details.
  GetFrame().GetDocument()->UpdateStyleAndLayoutIgnorePendingStylesheets();

  if (cancel)
    return;
  GetFrame().GetEditor().ReplaceSelectionWithText(
      text, false, false, InputEvent::InputType::kInsertReplacementText);
}

void SpellChecker::RespondToChangedSelection() {
  idle_spell_check_callback_->SetNeedsInvocation();
}

void SpellChecker::RespondToChangedContents() {
  idle_spell_check_callback_->SetNeedsInvocation();
}

void SpellChecker::RemoveSpellingMarkers() {
  GetFrame().GetDocument()->Markers().RemoveMarkersOfTypes(
      DocumentMarker::MisspellingMarkers());
}

void SpellChecker::RemoveSpellingMarkersUnderWords(
    const Vector<String>& words) {
  DocumentMarkerController& marker_controller =
      GetFrame().GetDocument()->Markers();
  marker_controller.RemoveSpellingMarkersUnderWords(words);
  marker_controller.RepaintMarkers();
}

static Node* FindFirstMarkable(Node* node) {
  while (node) {
    if (!node->GetLayoutObject())
      return nullptr;
    if (node->GetLayoutObject()->IsText())
      return node;
    if (node->GetLayoutObject()->IsTextControl())
      node = ToLayoutTextControl(node->GetLayoutObject())
                 ->GetTextControlElement()
                 ->VisiblePositionForIndex(1)
                 .DeepEquivalent()
                 .AnchorNode();
    else if (node->hasChildren())
      node = node->firstChild();
    else
      node = node->nextSibling();
  }

  return nullptr;
}

bool SpellChecker::SelectionStartHasMarkerFor(
    DocumentMarker::MarkerType marker_type,
    int from,
    int length) const {
  Node* node = FindFirstMarkable(GetFrame()
                                     .Selection()
                                     .ComputeVisibleSelectionInDOMTree()
                                     .Start()
                                     .AnchorNode());
  if (!node)
    return false;

  unsigned start_offset = static_cast<unsigned>(from);
  unsigned end_offset = static_cast<unsigned>(from + length);
  DocumentMarkerVector markers =
      GetFrame().GetDocument()->Markers().MarkersFor(node);
  for (size_t i = 0; i < markers.size(); ++i) {
    DocumentMarker* marker = markers[i];
    if (marker->StartOffset() <= start_offset &&
        end_offset <= marker->EndOffset() && marker->GetType() == marker_type)
      return true;
  }

  return false;
}

void SpellChecker::RemoveMarkers(const EphemeralRange& range,
                                 DocumentMarker::MarkerTypes marker_types) {
  DCHECK(!GetFrame().GetDocument()->NeedsLayoutTreeUpdate());

  if (range.IsNull())
    return;

  GetFrame().GetDocument()->Markers().RemoveMarkersInRange(range, marker_types);
}

void SpellChecker::CancelCheck() {
  spell_check_requester_->CancelCheck();
}

void SpellChecker::DocumentAttached(Document* document) {
  idle_spell_check_callback_->DocumentAttached(document);
}

void SpellChecker::Trace(blink::Visitor* visitor) {
  visitor->Trace(frame_);
  visitor->Trace(spell_check_requester_);
  visitor->Trace(idle_spell_check_callback_);
}

void SpellChecker::PrepareForLeakDetection() {
  spell_check_requester_->PrepareForLeakDetection();
  idle_spell_check_callback_->Deactivate();
}

Vector<TextCheckingResult> SpellChecker::FindMisspellings(const String& text) {
  Vector<UChar> characters;
  text.AppendTo(characters);
  unsigned length = text.length();

  TextBreakIterator* iterator = WordBreakIterator(characters.data(), length);
  if (!iterator)
    return Vector<TextCheckingResult>();

  Vector<TextCheckingResult> results;
  int word_start = iterator->current();
  while (word_start >= 0) {
    int word_end = iterator->next();
    if (word_end < 0)
      break;
    int word_length = word_end - word_start;
    int misspelling_location = -1;
    int misspelling_length = 0;
    if (WebTextCheckClient* text_checker_client = GetTextCheckerClient()) {
      // SpellCheckWord will write (0, 0) into the output vars, which is what
      // our caller expects if the word is spelled correctly.
      text_checker_client->CheckSpelling(
          String(characters.data() + word_start, word_length),
          misspelling_location, misspelling_length, nullptr);
    } else {
      misspelling_location = 0;
    }
    if (misspelling_length > 0) {
      DCHECK_GE(misspelling_location, 0);
      DCHECK_LE(misspelling_location + misspelling_length, word_length);
      TextCheckingResult misspelling;
      misspelling.decoration = kTextDecorationTypeSpelling;
      misspelling.location = word_start + misspelling_location;
      misspelling.length = misspelling_length;
      results.push_back(misspelling);
    }
    word_start = word_end;
  }
  return results;
}

std::pair<String, int> SpellChecker::FindFirstMisspelling(const Position& start,
                                                          const Position& end) {
  String misspelled_word;

  // Initialize out parameters; they will be updated if we find something to
  // return.
  String first_found_item;
  int first_found_offset = 0;

  // Expand the search range to encompass entire paragraphs, since text checking
  // needs that much context. Determine the character offset from the start of
  // the paragraph to the start of the original search range, since we will want
  // to ignore results in this area.
  EphemeralRange paragraph_range =
      ExpandToParagraphBoundary(EphemeralRange(start, start));
  Position paragraph_start = paragraph_range.StartPosition();
  Position paragraph_end = paragraph_range.EndPosition();

  const int total_range_length =
      TextIterator::RangeLength(paragraph_start, end);
  const int range_start_offset =
      TextIterator::RangeLength(paragraph_start, start);
  int total_length_processed = 0;

  bool first_iteration = true;
  bool last_iteration = false;
  while (total_length_processed < total_range_length) {
    // Iterate through the search range by paragraphs, checking each one for
    // spelling.
    int current_length =
        TextIterator::RangeLength(paragraph_start, paragraph_end);
    int current_start_offset = first_iteration ? range_start_offset : 0;
    int current_end_offset = current_length;
    if (InSameParagraph(CreateVisiblePosition(paragraph_start),
                        CreateVisiblePosition(end))) {
      // Determine the character offset from the end of the original search
      // range to the end of the paragraph, since we will want to ignore results
      // in this area.
      current_end_offset = TextIterator::RangeLength(paragraph_start, end);
      last_iteration = true;
    }
    if (current_start_offset < current_end_offset) {
      String paragraph_string = PlainText(paragraph_range);
      if (paragraph_string.length() > 0) {
        int spelling_location = 0;

        Vector<TextCheckingResult> results = FindMisspellings(paragraph_string);

        for (unsigned i = 0; i < results.size(); i++) {
          const TextCheckingResult* result = &results[i];
          if (result->location >= current_start_offset &&
              result->location + result->length <= current_end_offset) {
            DCHECK_GT(result->length, 0);
            DCHECK_GE(result->location, 0);
            spelling_location = result->location;
            misspelled_word =
                paragraph_string.Substring(result->location, result->length);
            DCHECK(misspelled_word.length());
            break;
          }
        }

        if (!misspelled_word.IsEmpty()) {
          int spelling_offset = spelling_location - current_start_offset;
          if (!first_iteration)
            spelling_offset +=
                TextIterator::RangeLength(start, paragraph_start);
          first_found_offset = spelling_offset;
          first_found_item = misspelled_word;
          break;
        }
      }
    }
    if (last_iteration ||
        total_length_processed + current_length >= total_range_length)
      break;
    Position new_paragraph_start =
        StartOfNextParagraph(CreateVisiblePosition(paragraph_end))
            .DeepEquivalent();
    if (new_paragraph_start.IsNull())
      break;

    paragraph_range = ExpandToParagraphBoundary(
        EphemeralRange(new_paragraph_start, new_paragraph_start));
    paragraph_start = paragraph_range.StartPosition();
    paragraph_end = paragraph_range.EndPosition();
    first_iteration = false;
    total_length_processed += current_length;
  }
  return std::make_pair(first_found_item, first_found_offset);
}

// static
bool SpellChecker::IsSpellCheckingEnabledAt(const Position& position) {
  if (position.IsNull())
    return false;
  if (TextControlElement* text_control = EnclosingTextControl(position)) {
    if (auto* input = ToHTMLInputElementOrNull(text_control)) {
      // TODO(tkent): The following password type check should be done in
      // HTMLElement::spellcheck(). crbug.com/371567
      if (input->type() == InputTypeNames::password)
        return false;
      if (!input->IsFocusedElementInDocument())
        return false;
    }
  }
  HTMLElement* element =
      Traversal<HTMLElement>::FirstAncestorOrSelf(*position.AnchorNode());
  return element && element->IsSpellCheckingEnabled();
}

STATIC_ASSERT_ENUM(kWebTextDecorationTypeSpelling, kTextDecorationTypeSpelling);
STATIC_ASSERT_ENUM(kWebTextDecorationTypeGrammar, kTextDecorationTypeGrammar);

}  // namespace blink
