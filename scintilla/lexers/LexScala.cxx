// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//! Lexer for Scala, based on LexCoffeeScript.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>
#include <vector>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "StringUtils.h"
#include "LexerModule.h"
#include "LexerUtils.h"

using namespace Lexilla;

namespace {

struct EscapeSequence {
	int outerState = SCE_SCALA_DEFAULT;
	int digitsLeft = 0;

	// highlight any character as escape sequence.
	bool resetEscapeState(int state, int chNext) noexcept {
		if (IsEOLChar(chNext)) {
			return false;
		}
		outerState = state;
		digitsLeft = (chNext == 'u') ? 5 : 1;
		return true;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsHexDigit(ch);
	}
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Class = 1,
	KeywordIndex_Trait = 2,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

enum class KeywordType {
	None = SCE_SCALA_DEFAULT,
	Annotation = SCE_SCALA_ANNOTATION,
	Class = SCE_SCALA_CLASS,
	Trait = SCE_SCALA_TRAIT,
	Enum = SCE_SCALA_ENUM,
	Function = SCE_SCALA_FUNCTION_DEFINITION,
	Return = 0x40,
};

constexpr bool IsScalaIdentifierStart(int ch) noexcept {
	return IsIdentifierStartEx(ch) || ch == '$';
}

constexpr bool IsScalaIdentifierChar(int ch) noexcept {
	return IsIdentifierCharEx(ch) || ch == '$';
}

constexpr bool IsSingleLineString(int state) noexcept {
	return state <= SCE_SCALA_INTERPOLATED_STRING;
}

constexpr int GetStringQuote(int state) noexcept {
	return (state == SCE_SCALA_BACKTICKS) ? '`' : ((state < SCE_SCALA_XML_STRING_DQ) ? '\'' : '\"');
}

constexpr bool IsTripleString(int state) noexcept {
	return state == SCE_SCALA_TRIPLE_STRING || state == SCE_SCALA_TRIPLE_INTERPOLATED_STRING;
}

constexpr bool IsInterpolatedString(int state) noexcept {
	return state == SCE_SCALA_INTERPOLATED_STRING || state == SCE_SCALA_TRIPLE_INTERPOLATED_STRING;
}

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state <= SCE_SCALA_TASKMARKER;
}

constexpr bool FollowExpression(int chPrevNonWhite, int stylePrevNonWhite) noexcept {
	return chPrevNonWhite == ')' || chPrevNonWhite == ']'
		|| (stylePrevNonWhite >= SCE_SCALA_OPERATOR_PF && stylePrevNonWhite <= SCE_SCALA_IDENTIFIER)
		|| IsScalaIdentifierChar(chPrevNonWhite);
}

inline bool IsXmlTagStart(const StyleContext &sc, int chPrevNonWhite, int stylePrevNonWhite) noexcept {
	return ((sc.chPrev == '(' || sc.chPrev == '{')
		|| (sc.chPrev <= ' ' && (stylePrevNonWhite == SCE_SCALA_XML_TAG || stylePrevNonWhite == SCE_SCALA_WORD || !FollowExpression(chPrevNonWhite, stylePrevNonWhite))))
		&& (IsScalaIdentifierChar(sc.chNext) || sc.chNext == '!' || sc.chNext == '?');
}

void ColouriseScalaDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	KeywordType kwType = KeywordType::None;
	int commentLevel = 0;	// nested block comment level
	std::vector<int> nestedState; // string interpolation "${}"

	int lineState = 0;
	int visibleChars = 0;
	int indentCount = 0;
	int xmlTagLevel = 0;

	int chBefore = 0;
	int visibleCharsBefore = 0;
	int chPrevNonWhite = 0;
	int stylePrevNonWhite = SCE_SCALA_DEFAULT;
	EscapeSequence escSeq;

	if (startPos != 0) {
		// backtrack to the line starts XML or interpolation for better coloring on typing.
		BacktrackToStart(styler, PyLineStateStringInterpolation, startPos, lengthDoc, initStyle);
	}

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		lineState = styler.GetLineState(sc.currentLine - 1);
		commentLevel = (lineState >> 8) & 0xff;
		lineState = 0;
	}
	if (startPos == 0) {
		if (sc.Match('#', '!')) {
			// Shell Shebang at beginning of file
			sc.SetState(SCE_SCALA_COMMENTLINE);
			sc.Forward();
		}
	} else if (IsSpaceEquiv(initStyle)) {
		LookbackNonWhite(styler, startPos, SCE_SCALA_TASKMARKER, chPrevNonWhite, stylePrevNonWhite);
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_SCALA_OPERATOR:
		case SCE_SCALA_OPERATOR2:
		case SCE_SCALA_OPERATOR_PF:
			sc.SetState(SCE_SCALA_DEFAULT);
			break;

		case SCE_SCALA_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_SCALA_DEFAULT);
			}
			break;

		case SCE_SCALA_IDENTIFIER:
		case SCE_SCALA_ANNOTATION:
		case SCE_SCALA_SYMBOL:
		case SCE_SCALA_XML_TAG:
		case SCE_SCALA_XML_ATTRIBUTE:
			if ((sc.ch == '.' && !(sc.state == SCE_SCALA_IDENTIFIER || sc.state == SCE_SCALA_SYMBOL))
				|| (sc.ch == ':' && (sc.state == SCE_SCALA_XML_TAG || sc.state == SCE_SCALA_XML_ATTRIBUTE))) {
				const int state = sc.state;
				sc.SetState(SCE_SCALA_OPERATOR2);
				sc.ForwardSetState(state);
			}
			if (!IsScalaIdentifierChar(sc.ch) && !(sc.ch == '-' && (sc.state == SCE_SCALA_XML_TAG || sc.state == SCE_SCALA_XML_ATTRIBUTE))) {
				if (sc.state == SCE_SCALA_IDENTIFIER) {
					if (escSeq.outerState == SCE_SCALA_DEFAULT) {
						char s[128];
						sc.GetCurrent(s, sizeof(s));
						if (keywordLists[KeywordIndex_Keyword].InList(s)) {
							sc.ChangeState(SCE_SCALA_WORD);
							kwType = KeywordType::None;
							if (StrEqualsAny(s, "class", "new", "extends", "throws", "object")) {
								kwType = KeywordType::Class;
							} else if (StrEqualsAny(s, "trait", "with")) {
								kwType = KeywordType::Trait;
							} else if (StrEqual(s, "def")) {
								kwType = KeywordType::Function;
							} else if (StrEqual(s, "enum")) {
								kwType = KeywordType::Enum;
							} else if (StrEqualsAny(s, "return", "yield")) {
								kwType = KeywordType::Return;
							} else if (visibleChars == 3 && StrEqual(s, "end")) {
								lineState |= PyLineStateMaskCloseBrace;
							}
							if (kwType > KeywordType::None && kwType < KeywordType::Return) {
								const int chNext = sc.GetLineNextChar();
								if (!IsIdentifierStartEx(chNext)) {
									kwType = KeywordType::None;
								}
							}
						} else if (keywordLists[KeywordIndex_Class].InList(s)) {
							sc.ChangeState(SCE_SCALA_CLASS);
						} else if (keywordLists[KeywordIndex_Trait].InList(s)) {
							sc.ChangeState(SCE_SCALA_TRAIT);
						} else if (sc.ch != '.') {
							if (kwType > KeywordType::None && kwType < KeywordType::Return) {
								sc.ChangeState(static_cast<int>(kwType));
							} else {
								const int chNext = sc.GetLineNextChar();
								if (chNext == '(') {
									// type method()
									// type[] method()
									if (kwType != KeywordType::Return && (IsIdentifierCharEx(chBefore) || chBefore == ']')) {
										sc.ChangeState(SCE_SCALA_FUNCTION_DEFINITION);
									} else {
										sc.ChangeState(SCE_SCALA_FUNCTION);
									}
								}
							}
						}
						stylePrevNonWhite = sc.state;
						if (sc.state != SCE_SCALA_WORD && sc.ch != '.') {
							kwType = KeywordType::None;
						}
					} else {
						sc.SetState(escSeq.outerState);
						continue;
					}
				}
				sc.SetState((sc.state == SCE_SCALA_XML_TAG || sc.state == SCE_SCALA_XML_ATTRIBUTE) ? SCE_SCALA_XML_OTHER : SCE_SCALA_DEFAULT);
				continue;
			}
			break;

		case SCE_SCALA_COMMENTLINE:
			if (sc.atLineStart) {
				sc.SetState(SCE_SCALA_DEFAULT);
			} else {
				HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_SCALA_TASKMARKER);
			}
			break;

		case SCE_SCALA_COMMENTBLOCK:
		case SCE_SCALA_COMMENTBLOCKDOC:
			if (sc.atLineStart) {
				lineState  = PyLineStateMaskCommentLine;
			}
			if (sc.Match('*', '/')) {
				sc.Forward();
				--commentLevel;
				if (commentLevel == 0) {
					sc.ForwardSetState(SCE_SCALA_DEFAULT);
					if (lineState == PyLineStateMaskCommentLine && sc.GetLineNextChar() != '\0') {
						lineState = 0;
					}
				}
			} else if (sc.Match('/', '*')) {
				sc.Forward();
				++commentLevel;
			} else if (sc.state == SCE_SCALA_COMMENTBLOCKDOC && sc.ch == '@' && IsAlpha(sc.chNext) && IsCommentTagPrev(sc.chPrev)) {
				sc.SetState(SCE_SCALA_COMMENTTAG);
			} else if (HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_SCALA_TASKMARKER)) {
				continue;
			}
			break;

		case SCE_SCALA_COMMENTTAG:
			if (!IsAlpha(sc.ch)) {
				sc.SetState(SCE_SCALA_COMMENTBLOCKDOC);
				continue;
			}
			break;

		case SCE_SCALA_BACKTICKS:
		case SCE_SCALA_CHARACTER:
		case SCE_SCALA_XML_STRING_SQ:
		case SCE_SCALA_XML_STRING_DQ:
		case SCE_SCALA_STRING:
		case SCE_SCALA_INTERPOLATED_STRING:
		case SCE_SCALA_TRIPLE_STRING:
		case SCE_SCALA_TRIPLE_INTERPOLATED_STRING:
			if (sc.atLineStart && IsSingleLineString(sc.state)) {
				sc.SetState(SCE_SCALA_DEFAULT);
			} else if (sc.ch == '\\') {
				if (escSeq.resetEscapeState(sc.state, sc.chNext)) {
					sc.SetState(SCE_SCALA_ESCAPECHAR);
					sc.Forward();
					if (IsInterpolatedString(sc.state) && sc.Match('$', '\"')) {
						sc.Forward();
					}
				}
			} else if (sc.ch == '$' && IsInterpolatedString(sc.state)) {
				if (sc.chNext == '$') {
					escSeq.outerState = sc.state;
					escSeq.digitsLeft = 1;
					sc.SetState(SCE_SCALA_ESCAPECHAR);
					sc.Forward();
				} else if (sc.chNext == '{') {
					nestedState.push_back(sc.state);
					sc.SetState(SCE_SCALA_OPERATOR2);
					sc.Forward();
				} else if (IsScalaIdentifierStart(sc.chNext)) {
					escSeq.outerState = sc.state;
					sc.SetState(SCE_SCALA_IDENTIFIER);
				}
			} else if (sc.ch == GetStringQuote(sc.state) && (IsSingleLineString(sc.state) || sc.MatchNext('"', '"'))) {
				if (!IsSingleLineString(sc.state)) {
					// quotes except last three are string content
					while (sc.chNext == '\"') {
						sc.Forward();
					}
				}
				sc.ForwardSetState((sc.state == SCE_SCALA_XML_STRING_SQ || sc.state == SCE_SCALA_XML_STRING_DQ) ? SCE_SCALA_XML_OTHER : SCE_SCALA_DEFAULT);
				continue;
			}
			break;

		case SCE_SCALA_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_SCALA_XML_TEXT:
		case SCE_SCALA_XML_OTHER:
			if (sc.ch == '>' || sc.Match('/', '>')) {
				sc.SetState(SCE_SCALA_XML_TAG);
				if (sc.ch == '/') {
					// self closing <tag />
					--xmlTagLevel;
					sc.Forward();
				}
				chPrevNonWhite = '>';
				stylePrevNonWhite = SCE_SCALA_XML_TAG;
				sc.ForwardSetState((xmlTagLevel == 0) ? SCE_SCALA_DEFAULT : SCE_SCALA_XML_TEXT);
				continue;
			} else if (sc.ch == '=' && (sc.state == SCE_SCALA_XML_OTHER)) {
				sc.SetState(SCE_SCALA_OPERATOR2);
				sc.ForwardSetState(SCE_SCALA_XML_OTHER);
				continue;
			} else if ((sc.ch == '\'' || sc.ch == '\"') && (sc.state == SCE_SCALA_XML_OTHER)) {
				sc.SetState((sc.ch == '\'') ? SCE_SCALA_XML_STRING_SQ : SCE_SCALA_XML_STRING_DQ);
			} else if ((sc.state == SCE_SCALA_XML_OTHER) && IsScalaIdentifierStart(sc.ch)) {
				sc.SetState(SCE_SCALA_XML_ATTRIBUTE);
			} else if (sc.ch == '{') {
				nestedState.push_back(sc.state);
				sc.SetState(SCE_SCALA_OPERATOR2);
			} else if (sc.Match('<', '/')) {
				--xmlTagLevel;
				sc.SetState(SCE_SCALA_XML_TAG);
				sc.Forward();
			} else if (sc.ch == '<') {
				++xmlTagLevel;
				sc.SetState(SCE_SCALA_XML_TAG);
			}
			break;
		}

		if (sc.state == SCE_SCALA_DEFAULT) {
			if (sc.Match('/', '/')) {
				if (visibleChars == 0) {
					lineState = PyLineStateMaskCommentLine;
				}
				visibleCharsBefore = visibleChars;
				sc.SetState(SCE_SCALA_COMMENTLINE);
			} else if (sc.Match('/', '*')) {
				commentLevel = 1;
				visibleCharsBefore = visibleChars;
				if (visibleChars == 0) {
					lineState = PyLineStateMaskCommentLine;
				}
				sc.SetState(SCE_SCALA_COMMENTBLOCK);
				sc.Forward(2);
				if (sc.ch == '*' && sc.chNext != '*') {
					sc.ChangeState(SCE_SCALA_COMMENTBLOCKDOC);
				}
				continue;
			} else if (sc.ch == '\"') {
				const bool interpolated = stylePrevNonWhite != SCE_SCALA_NUMBER && IsScalaIdentifierChar(sc.chPrev);
				sc.SetState(interpolated ? SCE_SCALA_INTERPOLATED_STRING : SCE_SCALA_STRING);
				if (sc.MatchNext('"', '"')) {
					static_assert(SCE_SCALA_TRIPLE_INTERPOLATED_STRING - SCE_SCALA_INTERPOLATED_STRING == SCE_SCALA_TRIPLE_STRING - SCE_SCALA_STRING);
					sc.SetState(sc.state + SCE_SCALA_TRIPLE_STRING - SCE_SCALA_STRING);
					sc.Advance(2);
				}
			} else if (sc.ch == '\'') {
				int state = SCE_SCALA_CHARACTER;
				if (sc.chNext == '{' || IsScalaIdentifierStart(sc.chNext)) {
					const int after = sc.GetCharAfterNext();
					if (after != '\'') {
						state = (sc.chNext == '{') ? SCE_SCALA_OPERATOR : SCE_SCALA_SYMBOL;
					}
				}
				sc.SetState(state);
			} else if (sc.ch == '<') {
				// <tag></tag>
				if (sc.chNext == '/') {
					--xmlTagLevel;
					sc.SetState(SCE_SCALA_XML_TAG);
					sc.Forward();
				} else if (IsXmlTagStart(sc, chPrevNonWhite, stylePrevNonWhite)) {
					++xmlTagLevel;
					sc.SetState(SCE_SCALA_XML_TAG);
				} else {
					sc.SetState(SCE_SCALA_OPERATOR);
				}
			} else if (sc.ch == '`') {
				sc.SetState(SCE_SCALA_BACKTICKS);
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_SCALA_NUMBER);
			} else if (IsScalaIdentifierStart(sc.ch)) {
				escSeq.outerState = SCE_SCALA_DEFAULT;
				chBefore = chPrevNonWhite;
				sc.SetState(SCE_SCALA_IDENTIFIER);
			} else if (sc.ch == '@' && IsScalaIdentifierStart(sc.chNext)) {
				sc.SetState(SCE_SCALA_ANNOTATION);
			} else if (IsAGraphic(sc.ch)) {
				sc.SetState(SCE_SCALA_OPERATOR);
				if ((sc.ch == '+' || sc.ch == '-') && sc.ch == sc.chNext) {
					sc.ChangeState(SCE_SCALA_OPERATOR_PF);
					sc.Forward();
				} else if (!nestedState.empty()) {
					sc.ChangeState(SCE_SCALA_OPERATOR2);
					if (sc.ch == '{') {
						nestedState.push_back(SCE_SCALA_DEFAULT);
					} else if (sc.ch == '}') {
						const int outerState = TakeAndPop(nestedState);
						sc.ForwardSetState(outerState);
						continue;
					}
				} else if (visibleChars == 0 && (sc.ch == '}' || sc.ch == ']' || sc.ch == ')')) {
					lineState |= PyLineStateMaskCloseBrace;
				}
			}
		}

		if (visibleChars == 0 && IsASpaceOrTab(sc.ch)) {
			++indentCount;
		}
		if (!isspacechar(sc.ch)) {
			visibleChars++;
			if (!IsSpaceEquiv(sc.state)) {
				chPrevNonWhite = sc.ch;
				stylePrevNonWhite = sc.state;
			}
		}
		if (sc.atLineEnd) {
			if (!nestedState.empty() || xmlTagLevel != 0) {
				lineState = PyLineStateStringInterpolation | PyLineStateMaskTripleQuote;
			} else if (IsTripleString(sc.state)) {
				lineState = PyLineStateMaskTripleQuote;
			} else if (lineState == 0 && visibleChars == 0) {
				lineState = PyLineStateMaskEmptyLine;
			}
			lineState |= (commentLevel << 8) | (indentCount << 16);
			styler.SetLineState(sc.currentLine, lineState);
			lineState = 0;
			indentCount = 0;
			visibleChars = 0;
			visibleCharsBefore = 0;
			kwType = KeywordType::None;
		}
		sc.Forward();
	}

	sc.Complete();
}

// TODO: use brace based folding for Scala 2.
}

extern const LexerModule lmScala(SCLEX_SCALA, ColouriseScalaDoc, "scala", FoldPyDoc);
