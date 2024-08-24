// This file is part of Notepad4.
// See License.txt for details about distribution and modification.
//! Lexer for Zig

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>

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

using namespace Lexilla;

namespace {

// https://ziglang.org/documentation/master/#Escape-Sequences
struct EscapeSequence {
	int outerState = SCE_ZIG_DEFAULT;
	int digitsLeft = 0;
	bool brace = false;

	// highlight any character as escape sequence.
	void resetEscapeState(int state, int chNext) noexcept {
		outerState = state;
		digitsLeft = 1;
		brace = false;
		if (chNext == 'x') {
			digitsLeft = 3;
		} else if (chNext == 'u') {
			digitsLeft = 5;
		}
	}
	void resetEscapeState(int state) noexcept {
		outerState = state;
		digitsLeft = 1;
		brace = false;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsHexDigit(ch);
	}
};

// https://ziglang.org/documentation/master/std/#std.fmt.format
enum class FormatArgument {
	None,
	Digit,
	Identifier,
	Error,
};

constexpr bool IsBraceFormatSpecifier(int ch) noexcept {
	return AnyOf(ch, 'b',
					'c',
					'd',
					'e',
					'f',
					'g',
					'o',
					's',
					'u',
					'x', 'X',
					'?', '!', '*', 'a');
}

constexpr bool IsBraceFormatNext(int ch) noexcept {
	return ch == '}' || IsADigit(ch) || ch == '[' || ch == ':' || ch == '.'
		|| IsBraceFormatSpecifier(ch);
}

constexpr bool IsFormatArgument(int ch, FormatArgument fmtArgument) noexcept {
	return IsADigit(ch) || (fmtArgument == FormatArgument::Identifier && IsIdentifierCharEx(ch));
}

inline Sci_Position CheckBraceFormatSpecifier(const StyleContext &sc, LexAccessor &styler) noexcept {
	Sci_PositionU pos = sc.currentPos;
	char ch = static_cast<char>(sc.ch);
	// [specifier]
	if (IsBraceFormatSpecifier(sc.ch)) {
		++pos;
		if (sc.Match('a', 'n', 'y')) {
			pos += 2;
		}
		ch = styler[pos];
		if (!AnyOf(ch, ':', '.', '}', '<', '>', '^')) {
			return 0;
		}
	}
	if (ch == ':') {
		ch = styler[++pos];
	}
	// [[fill] alignment]
	if (!AnyOf(ch, '\r', '\n', '{', '}')) {
		Sci_Position width = 1;
		if (ch & 0x80) {
			styler.GetCharacterAndWidth(pos, &width);
		}
		const char chNext = styler[pos + width];
		if (AnyOf(ch, '<', '>', '^') || AnyOf(chNext, '<', '>', '^')) {
			pos += 1 + width;
			ch = styler[pos];
		}
	}
	// [width]
	while (IsADigit(ch)) {
		ch = styler[++pos];
	}
	// [.precision]
	if (ch == '.') {
		ch = styler[++pos];
		while (IsADigit(ch)) {
			ch = styler[++pos];
		}
	}
	if (ch == '}') {
		return pos - sc.currentPos;
	}
	return 0;
}

enum {
	ZigLineStateMaskLineComment = 1, // line comment
	ZigLineStateMaskMultilineString = 1 << 1, // multiline string
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Type = 1,
	MaxKeywordSize = 16,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

enum class KeywordType {
	None = SCE_ZIG_DEFAULT,
	Function = SCE_ZIG_FUNCTION_DEFINITION,
};

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state <= SCE_ZIG_TASKMARKER;
}

void ColouriseZigDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	KeywordType kwType = KeywordType::None;
	int visibleChars = 0;
	int lineState = 0;
	FormatArgument fmtArgument = FormatArgument::None;
	EscapeSequence escSeq;

	StyleContext sc(startPos, lengthDoc, initStyle, styler);

	while (sc.More()) {
		switch (sc.state) {
		case SCE_ZIG_OPERATOR:
			sc.SetState(SCE_ZIG_DEFAULT);
			break;

		case SCE_ZIG_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_ZIG_DEFAULT);
			}
			break;

		case SCE_ZIG_IDENTIFIER:
		case SCE_ZIG_BUILTIN_FUNCTION:
			if (!IsIdentifierCharEx(sc.ch)) {
				if (sc.state == SCE_ZIG_IDENTIFIER) {
					char s[MaxKeywordSize];
					sc.GetCurrent(s, sizeof(s));
					if (keywordLists[KeywordIndex_Keyword].InList(s)) {
						sc.ChangeState(SCE_ZIG_WORD);
						kwType = KeywordType::None;
						if (StrEqual(s, "fn")) {
							kwType = KeywordType::Function;
						}
					} else if (keywordLists[KeywordIndex_Type].InList(s)) {
						sc.ChangeState(SCE_ZIG_TYPE);
					} else if (kwType != KeywordType::None) {
						sc.ChangeState(static_cast<int>(kwType));
					} else if (sc.GetLineNextChar() == '(') {
						sc.ChangeState(SCE_ZIG_FUNCTION);
					}
				}
				if (sc.state != SCE_ZIG_WORD) {
					kwType = KeywordType::None;
				}
				sc.SetState(SCE_ZIG_DEFAULT);
			}
			break;

		case SCE_ZIG_CHARACTER:
		case SCE_ZIG_STRING:
		case SCE_ZIG_MULTISTRING:
			if (sc.atLineStart) {
				sc.SetState(SCE_ZIG_DEFAULT);
			} else if (sc.ch == '\\' && sc.state != SCE_ZIG_MULTISTRING) {
				escSeq.resetEscapeState(sc.state, sc.chNext);
				sc.SetState(SCE_ZIG_ESCAPECHAR);
				sc.Forward();
				if (sc.Match('u', '{')) {
					escSeq.brace = true;
					escSeq.digitsLeft = 7;
					sc.Forward();
				}
			} else if ((sc.ch == '\'' && sc.state == SCE_ZIG_CHARACTER) || (sc.ch == '\"' && sc.state == SCE_ZIG_STRING)) {
				sc.ForwardSetState(SCE_ZIG_DEFAULT);
			} else if (sc.state != SCE_ZIG_CHARACTER) {
				if (sc.ch == '{' || sc.ch == '}') {
					if (sc.ch == sc.chNext) {
						escSeq.resetEscapeState(sc.state);
						sc.SetState(SCE_ZIG_ESCAPECHAR);
						sc.Forward();
                    } else if (sc.ch == '{' && IsBraceFormatNext(sc.chNext)) {
                    	escSeq.outerState = sc.state;
                    	sc.SetState(SCE_ZIG_PLACEHOLDER);
                    	fmtArgument = FormatArgument::None;
                    	if (IsADigit(sc.chNext)) {
                    		fmtArgument = FormatArgument::Digit;
                    	} else if (sc.chNext == '[') {
                    		fmtArgument = FormatArgument::Identifier;
							if (IsIdentifierStartEx(sc.GetRelative(2))) {
								sc.Forward();
							}
                    	}
                    }
                }
			}
			break;

		case SCE_ZIG_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				if (escSeq.brace && sc.ch == '}') {
					sc.Forward();
				}
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_ZIG_PLACEHOLDER:
			if (!IsFormatArgument(sc.ch, fmtArgument)) {
				if (fmtArgument == FormatArgument::Identifier) {
					if (sc.ch == ']') {
						sc.Forward();
					} else {
						fmtArgument = FormatArgument::Error;
					}
				}
				if (fmtArgument != FormatArgument::Error) {
					const Sci_Position length = CheckBraceFormatSpecifier(sc, styler);
					if (length != 0) {
						sc.SetState(SCE_ZIG_FORMAT_SPECIFIER);
						sc.Advance(length);
						sc.SetState(SCE_ZIG_PLACEHOLDER);
						sc.ForwardSetState(escSeq.outerState);
						continue;
					}
				}
				if (fmtArgument == FormatArgument::Error || sc.ch != '}') {
					sc.Rewind();
					sc.ChangeState(escSeq.outerState);
				}
				sc.ForwardSetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_ZIG_COMMENTLINE:
		case SCE_ZIG_COMMENTLINEDOC:
		case SCE_ZIG_COMMENTLINETOP:
			if (sc.atLineStart) {
				sc.SetState(SCE_ZIG_DEFAULT);
			}
			break;
		}

		if (sc.state == SCE_ZIG_DEFAULT) {
			if (sc.Match('/', '/')) {
				if (visibleChars == 0) {
					lineState = ZigLineStateMaskLineComment;
				}
				sc.SetState(SCE_ZIG_COMMENTLINE);
				sc.Forward(2);
				if (sc.ch == '!') {
					sc.ChangeState(SCE_ZIG_COMMENTLINETOP);
				} else if (sc.ch == '/' && sc.chNext != '/') {
					sc.ChangeState(SCE_ZIG_COMMENTLINEDOC);
				}
			} else if (sc.Match('\\', '\\')) {
				lineState = ZigLineStateMaskMultilineString;
				sc.SetState(SCE_ZIG_MULTISTRING);
			} else if (sc.ch == '\"') {
				sc.SetState(SCE_ZIG_STRING);
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_ZIG_CHARACTER);
			} else if (IsNumberStart(sc.ch, sc.chNext)) {
				sc.SetState(SCE_ZIG_NUMBER);
			} else if ((sc.ch == '@' && IsIdentifierStartEx(sc.chNext)) || IsIdentifierStartEx(sc.ch)) {
				sc.SetState((sc.ch == '@') ? SCE_ZIG_BUILTIN_FUNCTION : SCE_ZIG_IDENTIFIER);
			} else if (IsAGraphic(sc.ch)) {
				sc.SetState(SCE_ZIG_OPERATOR);
			}
		}

		if (visibleChars == 0 && !isspacechar(sc.ch)) {
			visibleChars++;
		}
		if (sc.atLineEnd) {
			styler.SetLineState(sc.currentLine, lineState);
			lineState = 0;
			kwType = KeywordType::None;
			visibleChars = 0;
		}
		sc.Forward();
	}

	sc.Complete();
}

struct FoldLineState {
	int lineComment;
	int multilineString;
	constexpr explicit FoldLineState(int lineState) noexcept:
		lineComment(lineState & ZigLineStateMaskLineComment),
		multilineString((lineState >> 1) & 1) {
	}
};

void FoldZigDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList /*keywordLists*/, Accessor &styler) {
	const Sci_PositionU endPos = startPos + lengthDoc;
	Sci_Line lineCurrent = styler.GetLine(startPos);
	FoldLineState foldPrev(0);
	int levelCurrent = SC_FOLDLEVELBASE;
	if (lineCurrent > 0) {
		levelCurrent = styler.LevelAt(lineCurrent - 1) >> 16;
		foldPrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
		const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent - 1, SCE_ZIG_OPERATOR, SCE_ZIG_TASKMARKER);
		if (bracePos) {
			startPos = bracePos + 1; // skip the brace
		}
	}

	int levelNext = levelCurrent;
	FoldLineState foldCurrent(styler.GetLineState(lineCurrent));
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = sci::min(lineStartNext, endPos);
	int visibleChars = 0;

	while (startPos < endPos) {
		initStyle = styler.StyleIndexAt(startPos);

		if (initStyle == SCE_ZIG_OPERATOR) {
			const char ch = styler[startPos];
			if (ch == '{' || ch == '[' || ch == '(') {
				levelNext++;
			} else if (ch == '}' || ch == ']' || ch == ')') {
				levelNext--;
			}
		}

		if (visibleChars == 0 && !IsSpaceEquiv(initStyle)) {
			++visibleChars;
		}
		++startPos;
		if (startPos == lineStartNext) {
			const FoldLineState foldNext(styler.GetLineState(lineCurrent + 1));
			levelNext = sci::max(levelNext, SC_FOLDLEVELBASE);
			if (foldCurrent.lineComment) {
				levelNext += foldNext.lineComment - foldPrev.lineComment;
			} else if (foldCurrent.multilineString) {
				levelNext += foldNext.multilineString - foldPrev.multilineString;
			} else if (visibleChars) {
				const Sci_PositionU bracePos = CheckBraceOnNextLine(styler, lineCurrent, SCE_ZIG_OPERATOR, SCE_ZIG_TASKMARKER);
				if (bracePos) {
					levelNext++;
					startPos = bracePos + 1; // skip the brace
				}
			}

			const int levelUse = levelCurrent;
			int lev = levelUse | (levelNext << 16);
			if (levelUse < levelNext) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			styler.SetLevel(lineCurrent, lev);

			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = sci::min(lineStartNext, endPos);
			levelCurrent = levelNext;
			foldPrev = foldCurrent;
			foldCurrent = foldNext;
			visibleChars = 0;
		}
	}
}

}

extern const LexerModule lmZig(SCLEX_ZIG, ColouriseZigDoc, "zig", FoldZigDoc);
