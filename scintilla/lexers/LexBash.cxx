// Scintilla source code edit control
/** @file LexBash.cxx
 ** Lexer for Bash.
 **/
// Copyright 2004-2012 by Neil Hodgson <neilh@scintilla.org>
// Adapted from LexPerl by Kein-Hong Man 2004
// The License.txt file describes the conditions under which this software may be distributed.

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

enum class ShellDialect {
	Bash,
	CShell,
	M4,
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_BashStruct = 1,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

#define HERE_DELIM_MAX			256

// state constants for parts of a bash command segment
enum class CmdState : uint8_t {
	Body,
	Start,
	Word,
	Test,			// test
	SingleBracket,	// []
	DoubleBracket,	// [[]]
	Arithmetic,
	Delimiter,
};

// state constants for nested delimiter pairs, used by
// SCE_SH_STRING, SCE_SH_PARAM and SCE_SH_BACKTICKS processing
enum class QuoteStyle : uint8_t {
	Literal,		// ''
	CString,		// $''
	String,			// ""
	LString,		// $""
	HereDoc,		// here document
	Backtick,		// ``
	Parameter,		// ${}
	Command,		// $()
	Arithmetic,		// $(()), $[]
};

#define BASH_QUOTE_STACK_MAX	7

// https://www.gnu.org/software/bash/manual/bash.html#Shell-Arithmetic
constexpr int translateBashDigit(int ch) noexcept {
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'z') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'Z') {
		return ch - 'A' + 36;
	}
	if (ch == '@') {
		return 62;
	}
	if (ch == '_') {
		return 63;
	}
	return 64;
}

constexpr bool IsBashNumber(int digit, int base) noexcept {
	return digit < base || (digit >= 36 && base <= 36 && digit - 26 < base);
}

constexpr int opposite(int ch) noexcept {
	if (ch == '(') {
		return ')';
	}
	if (AnyOf<'[', '{'>(ch)) {
		return ch + 2;
	}
	return ch;
}

int GlobScan(const StyleContext &sc) noexcept {
	// forward scan for zsh globs, disambiguate versus bash arrays
	// complex expressions may still fail, e.g. unbalanced () '' "" etc
	int sLen = 0;
	int pCount = 0;
	int hash = 0;
	Sci_Position pos = sc.currentPos;
	while (true) {
		++sLen;
		const uint8_t c = sc.styler[++pos];
		if (c <= ' ') {
			return 0;
		}
		if (c == '\'' || c == '\"') {
			if (hash != 2) {
				return 0;
			}
		} else if (c == '#' && hash == 0) {
			hash = (sLen == 1) ? 2 : 1;
		} else if (c == '(') {
			pCount++;
		} else if (c == ')') {
			if (pCount == 0) {
				if (hash) {
					return sLen;
				}
				return 0;
			}
			pCount--;
		}
	}
	return 0;
}

constexpr bool IsBashWordChar(int ch) noexcept {
	// note that [+-] are often parts of identifiers in shell scripts
	return IsIdentifierChar(ch) || ch == '.' || ch == '+' || ch == '-';
}

constexpr bool IsBashWordChar(int ch, CmdState cmdState) noexcept {
	return IsIdentifierChar(ch) || (cmdState !=  CmdState::Arithmetic && (ch == '.' || ch == '+' || ch == '-'));
}

constexpr bool IsBashMetaCharacter(int ch) noexcept {
	return ch <= 32 || AnyOf(ch, '|', '&', ';', '(', ')', '<', '>');
}

//constexpr bool IsBashOperator(int ch) noexcept {
//	return AnyOf(ch, '^', '&', '%', '(', ')', '-', '+', '=', '|', '{', '}', '[', ']', ':', ';', '>', ',', '*', '<', '?', '!', '.', '~', '@');
//}

constexpr bool IsTestOperator(const char *s) noexcept {
	return s[2] == '\0' || (s[3] == '\0' && IsLowerCase(s[1]) && IsLowerCase(s[2]));
}

constexpr bool IsBashParamChar(int ch) noexcept {
	return IsIdentifierChar(ch);
}

constexpr bool IsBashParamStart(int ch, ShellDialect dialect) noexcept {
	// https://www.gnu.org/software/bash/manual/bash.html#Special-Parameters
	// https://zsh.sourceforge.io/Doc/Release/Parameters.html#Parameters
	// https://man.archlinux.org/man/dash.1#Special_Parameters
	// https://man.archlinux.org/man/ksh.1#Parameter_Expansion.
	return IsBashParamChar(ch) || AnyOf(ch, '*', '@', '#', '?', '-', '$', '!')
	// https://man.archlinux.org/man/tcsh.1.en#Variable_substitution_without_modifiers
		|| (dialect == ShellDialect::CShell && AnyOf(ch, '%', '<'))
	;
}

constexpr bool IsBashHereDoc(int ch) noexcept {
	return IsAlpha(ch) || AnyOf(ch, '_', '\\', '-', '+', '!', '%', '*', ',', '.', '/', ':', '?', '@', '[', ']', '^', '`', '{', '}', '~');
}

constexpr bool IsBashHereDoc2(int ch) noexcept {
	return IsAlphaNumeric(ch) || AnyOf(ch, '_', '-', '+', '!', '%', '*', ',', '.', '/', ':', '=', '?', '@', '[', ']', '^', '`', '{', '}', '~');
}

constexpr bool IsBashLeftShift(int ch) noexcept {
	return IsADigit(ch) || ch == '$';
}

constexpr bool IsBashCmdDelimiter(int ch) noexcept {
	return AnyOf(ch, '|', '&', ';', '(', ')', '{', '}');
}

constexpr bool IsBashCmdDelimiter(int ch, int chNext) noexcept {
	return (ch == chNext && (ch == '|' || ch == '&' || ch == ';'))
		|| (ch == '|' && chNext == '&');
}

constexpr bool StyleForceBacktrack(int state) noexcept {
	return AnyOf(state, SCE_SH_HERE_Q, SCE_SH_STRING_SQ, SCE_SH_STRING_DQ, SCE_SH_PARAM, SCE_SH_BACKTICKS);
}

class QuoteCls {	// Class to manage quote pairs (simplified vs LexPerl)
public:
	int Count = 0;
	int Up = '\0';
	int Down = '\0';
	QuoteStyle Style = QuoteStyle::Literal;
	uint8_t Outer = SCE_SH_DEFAULT;
	CmdState State = CmdState::Body;
	uint8_t unused = 0;
	void Clear() noexcept {
		Count = 0;
		Up	  = '\0';
		Down  = '\0';
		Style = QuoteStyle::Literal;
		Outer = SCE_SH_DEFAULT;
		State = CmdState::Body;
		unused = 0;
	}
	void Start(int u, QuoteStyle s, int outer, CmdState state) noexcept {
		Count = 1;
		Up    = u;
		Down  = opposite(Up);
		Style = s;
		Outer = static_cast<uint8_t>(outer);
		State = state;
	}
};

class QuoteStackCls {	// Class to manage quote pairs that nest
public:
	unsigned Depth = 0;
	int State = SCE_SH_DEFAULT;
	unsigned backtickLevel = 0;
	ShellDialect dialect = ShellDialect::Bash;
	QuoteCls Current;
	QuoteCls Stack[BASH_QUOTE_STACK_MAX];
	bool lineContinuation = false;
	[[nodiscard]] bool Empty() const noexcept {
		return Current.Up == '\0';
	}
	void Start(int u, QuoteStyle s, int outer, CmdState state) noexcept {
		if (Empty()) {
			Current.Start(u, s, outer, state);
			if (s == QuoteStyle::Backtick) {
				++backtickLevel;
			}
		} else {
			Push(u, s, outer, state);
		}
	}
	void Push(int u, QuoteStyle s, int outer, CmdState state) noexcept {
		if (Depth >= BASH_QUOTE_STACK_MAX) {
			return;
		}
		Stack[Depth] = Current;
		Depth++;
		Current.Start(u, s, outer, state);
		if (s == QuoteStyle::Backtick) {
			++backtickLevel;
		}
	}
	void Pop() noexcept {
		if (Depth == 0) {
			Clear();
			return;
		}
		Depth--;
		Current = Stack[Depth];
	}
	void Clear() noexcept {
		Depth = 0;
		State = SCE_SH_DEFAULT;
		backtickLevel = 0;
		Current.Clear();
	}
	bool CountDown(StyleContext &sc, CmdState &cmdState) {
		Current.Count--;
		while (Current.Count > 0 && sc.chNext == Current.Down) {
			Current.Count--;
			sc.Forward();
		}
		if (Current.Count == 0) {
			cmdState = Current.State;
			const int outer = Current.Outer;
			if (backtickLevel != 0 && Current.Style == QuoteStyle::Backtick) {
				--backtickLevel;
			}
			Pop();
			sc.ForwardSetState(outer);
			return true;
		}
		return false;
	}
	void Expand(StyleContext &sc, CmdState &cmdState) {
		const CmdState current = cmdState;
		const int state = sc.state;
		QuoteStyle style = QuoteStyle::Literal;
		State = state;
		sc.SetState(SCE_SH_SCALAR);
		sc.Forward();
		if (sc.ch == '{') {
			style = QuoteStyle::Parameter;
			sc.ChangeState(SCE_SH_PARAM);
		} else if (sc.ch == '\'') {
			style = QuoteStyle::CString;
			sc.ChangeState(SCE_SH_STRING_DQ);
		} else if (sc.ch == '"') {
			style = QuoteStyle::LString;
			sc.ChangeState(SCE_SH_STRING_DQ);
		} else if (sc.ch == '(' || sc.ch == '[') {
			sc.ChangeState(SCE_SH_OPERATOR);
			if (sc.ch == '[' || sc.chNext == '(') {
				style = QuoteStyle::Arithmetic;
				cmdState = CmdState::Arithmetic;
			} else {
				style = QuoteStyle::Command;
				cmdState = CmdState::Delimiter;
			}
		} else {
			// scalar has no delimiter pair
			if (!IsBashParamStart(sc.ch, dialect)) {
				sc.ChangeState(state); // not scalar
			}
			return;
		}
		Start(sc.ch, style, state, current);
		sc.Forward();
	}
	void Escape(StyleContext &sc) {
		unsigned count = 1;
		while (sc.chNext == '\\') {
			++count;
			sc.Forward();
		}
		bool escaped = count & 1U; // odd backslash escape next character
		if (escaped && IsEOLChar(sc.chNext)) {
			lineContinuation = true;
			if (sc.state == SCE_SH_IDENTIFIER) {
				sc.SetState(SCE_SH_OPERATOR);
			}
			return;
		}
		if (backtickLevel > 0 && dialect != ShellDialect::CShell) {
			// see https://github.com/ScintillaOrg/lexilla/issues/194
			/*
			for $k$ level substitution with $N$ backslashes:
			* when $N/2^k$ is odd, following dollar is escaped.
			* when $(N - 1)/2^k$ is even, following quote is escaped.
			* when $N = n\times 2^{k + 1} - 1$, following backtick is escaped.
			* when $N = n\times 2^{k + 1} + 2^k - 1$, following backtick starts inner substitution.
			* when $N = m\times 2^k + 2^{k - 1} - 1$ and $k > 1$, following backtick ends current substitution.
			*/
			if (sc.chNext == '$') {
				escaped = (count >> backtickLevel) & 1U;
			} else if (sc.chNext == '\"' || sc.chNext == '\'') {
				escaped = (((count - 1) >> backtickLevel) & 1U) == 0;
			} else if (sc.chNext == '`' && escaped) {
				unsigned mask = 1U << (backtickLevel + 1);
				count += 1;
				escaped = (count & (mask - 1)) == 0;
				if (!escaped) {
					unsigned remain = count - (mask >> 1U);
					if (static_cast<int>(remain) >= 0 && (remain & (mask - 1)) == 0) {
						escaped = true;
						++backtickLevel;
					} else if (backtickLevel > 1) {
						mask >>= 1U;
						remain = count - (mask >> 1U);
						if (static_cast<int>(remain) >= 0 && (remain & (mask - 1)) == 0) {
							escaped = true;
							--backtickLevel;
						}
					}
				}
			}
		}
		if (escaped) {
			sc.Forward();
		}
	}
};

void ColouriseBashDoc(Sci_PositionU startPos, Sci_Position length, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	class HereDocCls {	// Class to manage HERE document elements
	public:
		int State = 0;		// 0: '<<' encountered
		// 1: collect the delimiter
		// 2: here doc text (lines after the delimiter)
		int Quote = '\0';		// the char after '<<'
		bool Quoted = false;		// true if Quote in ('\'','"','`')
		bool Escaped = false;		// backslash in delimiter, common in configure script
		bool Indent = false;		// indented delimiter (for <<-)
		int DelimiterLength = 0;	// strlen(Delimiter)
		char Delimiter[HERE_DELIM_MAX]{};	// the Delimiter
		void Append(int ch) noexcept {
			Delimiter[DelimiterLength++] = static_cast<char>(ch);
			Delimiter[DelimiterLength] = '\0';
		}
	};
	HereDocCls HereDoc;

	QuoteStackCls QuoteStack;
	memset(&HereDoc, 0, sizeof(HereDoc));
	memset(&QuoteStack, 0, sizeof(QuoteStack));

	int numBase = 0;
	const Sci_PositionU endPos = startPos + length;
	CmdState cmdState = CmdState::Start;

	QuoteStack.dialect = static_cast<ShellDialect>(styler.GetPropertyInt("lexer.lang"));
	// Always backtracks to the start of a line that is not a continuation
	// of the previous line (i.e. start of a bash command segment)
	Sci_Line ln = styler.GetLine(startPos);
	while (ln != 0) {
		ln--;
		if (ln == 0 || styler.GetLineState(ln) == static_cast<int>(CmdState::Start)) {
			break;
		}
	}
	initStyle = SCE_SH_DEFAULT;
	startPos = styler.LineStart(ln);
	StyleContext sc(startPos, endPos - startPos, initStyle, styler);

	while (sc.More()) {
		// handle line continuation, updates per-line stored state
		if (sc.atLineStart) {
			CmdState state = CmdState::Body;	// force backtrack while retaining cmdState
			if (!StyleForceBacktrack(sc.state)) {
				// retain last line's state
				// arithmetic expression and double bracket test can span multiline without line continuation
				if (!QuoteStack.lineContinuation && !AnyOf(cmdState, CmdState::DoubleBracket, CmdState::Arithmetic)) {
					cmdState = CmdState::Start;
				}
				if (QuoteStack.Empty()) {	// force backtrack when nesting
					state = cmdState;
				}
			}
			QuoteStack.lineContinuation = false;
			styler.SetLineState(sc.currentLine, static_cast<int>(state));
		}

		// controls change of cmdState at the end of a non-whitespace element
		// states Body|Test|Arithmetic persist until the end of a command segment
		// state Word persist, but ends with 'in' or 'do' construct keywords
		CmdState cmdStateNew = CmdState::Body;
		if (cmdState >= CmdState::Word && cmdState <= CmdState::Arithmetic) {
			cmdStateNew = cmdState;
		}
		const int stylePrev = sc.state;

		// Determine if the current state should terminate.
		switch (sc.state) {
		case SCE_SH_OPERATOR:
			sc.SetState(SCE_SH_DEFAULT);
			if (cmdState == CmdState::Delimiter) {		// if command delimiter, start new command
				cmdStateNew = CmdState::Start;
			} else if (sc.chPrev == '\\') {			// propagate command state if line continued
				cmdStateNew = cmdState;
			}
			break;
		case SCE_SH_WORD:
			// "." never used in Bash variable names but used in file names
			if (!IsBashWordChar(sc.ch) || sc.Match('+', '=') || sc.Match('.', '.')) {
				char s[128];
				sc.GetCurrent(s, sizeof(s));
				// allow keywords ending in a whitespace, meta character or command delimiter
				const bool keywordEnds = IsBashMetaCharacter(sc.ch) || AnyOf(sc.ch, '{', '}')
					|| (QuoteStack.dialect == ShellDialect::M4 && AnyOf<'[', ']'>(sc.ch));
				// 'in' or 'do' may be construct keywords
				if (cmdState == CmdState::Word) {
					if (StrEqual(s, "in") && keywordEnds) {
						cmdStateNew = CmdState::Body;
					} else if (StrEqual(s, "do") && keywordEnds) {
						cmdStateNew = CmdState::Start;
					} else {
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
					sc.SetState(SCE_SH_DEFAULT);
					break;
				}
				// a 'test' keyword starts a test expression
				if (StrEqual(s, "test")) {
					if (cmdState == CmdState::Start && keywordEnds) {
						cmdStateNew = CmdState::Test;
					} else {
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
				}
				// detect bash construct keywords
				else if (keywordLists[KeywordIndex_BashStruct].InList(s)) {
					if (cmdState == CmdState::Start && keywordEnds) {
						cmdStateNew = CmdState::Start;
					} else {
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
				}
				// 'for'|'case'|'select' needs 'in'|'do' to be highlighted later
				else if (StrEqualsAny(s, "for", "case", "select")) {
					if (cmdState == CmdState::Start && keywordEnds) {
						cmdStateNew = CmdState::Word;
					} else {
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
				}
				// disambiguate option items and file test operators
				else if (s[0] == '-') {
					if (!AnyOf(cmdState, CmdState::Test, CmdState::SingleBracket, CmdState::DoubleBracket)
						|| !keywordEnds || !IsTestOperator(s)) {
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
				}
				// disambiguate keywords and identifiers
				else if (cmdState != CmdState::Start
					|| !keywordEnds || !keywordLists[KeywordIndex_Keyword].InList(s)) {
					sc.ChangeState(SCE_SH_IDENTIFIER);
				}

				if (StrEqual(s, "dnl")) { // m4
					sc.ChangeState(SCE_SH_COMMENTLINE);
					if (sc.atLineEnd) {
						sc.SetState(SCE_SH_DEFAULT);
					}
				} else {
					if (sc.state == SCE_SH_IDENTIFIER && (sc.ch == '(' || (sc.ch <= ' ' && sc.chNext == '('))) {
						sc.ChangeState(SCE_SH_FUNCTION);
					}
					sc.SetState(SCE_SH_DEFAULT);
				}
			}
			break;
		case SCE_SH_IDENTIFIER:
			if (!IsBashWordChar(sc.ch, cmdState)) {
				sc.SetState(SCE_SH_DEFAULT);
			}
			break;
		case SCE_SH_NUMBER: {
			const int digit = translateBashDigit(sc.ch);
			if (!IsBashNumber(digit, numBase)) {
				if (digit < 62 || digit == 63/* || (cmdState != CmdState::Arithmetic
					&& (sc.ch == '-' || (sc.ch == '.' && sc.chNext != '.')))*/) {
					// current character is alpha numeric, underscore, hyphen or dot
					sc.ChangeState(SCE_SH_IDENTIFIER);
				} else {
					sc.SetState(SCE_SH_DEFAULT);
				}
			}
		} break;
		case SCE_SH_COMMENTLINE:
			if (sc.MatchLineEnd()) {
				sc.SetState(SCE_SH_DEFAULT);
			}
			break;
		case SCE_SH_HERE_DELIM:
			// From Bash info:
			// ---------------
			// Specifier format is: <<[-]WORD
			// Optional '-' is for removal of leading tabs from here-doc.
			// Whitespace acceptable after <<[-] operator
			//
			if (HereDoc.State == 0) { // '<<' encountered
				HereDoc.Quote = sc.chNext;
				HereDoc.Quoted = false;
				HereDoc.Escaped = false;
				HereDoc.DelimiterLength = 0;
				HereDoc.Delimiter[0] = '\0';
				if (sc.chNext == '\'' || sc.chNext == '\"') {	// a quoted here-doc delimiter (' or ")
					sc.Forward();
					HereDoc.Quoted = true;
					HereDoc.State = 1;
				} else if (IsBashHereDoc(sc.chNext) ||
					(sc.chNext == '=' && cmdState != CmdState::Arithmetic)) {
					// an unquoted here-doc delimiter, no special handling
					HereDoc.State = 1;
				} else if (sc.chNext == '<') {	// HERE string <<<
					sc.Forward();
					sc.ForwardSetState(SCE_SH_DEFAULT);
				} else if (IsASpace(sc.chNext)) {
					// eat whitespace
				} else if (IsBashLeftShift(sc.chNext) ||
					(sc.chNext == '=' && cmdState == CmdState::Arithmetic)) {
					// left shift <<$var or <<= cases
					sc.ChangeState(SCE_SH_OPERATOR);
					sc.ForwardSetState(SCE_SH_DEFAULT);
				} else {
					// symbols terminates; deprecated zero-length delimiter
					HereDoc.State = 1;
				}
			} else if (HereDoc.State == 1) { // collect the delimiter
				// * if single quoted, there's no escape
				// * if double quoted, there are \\ and \" escapes
				if ((HereDoc.Quote == '\'' && sc.ch != HereDoc.Quote) ||
					(HereDoc.Quoted && sc.ch != HereDoc.Quote && sc.ch != '\\') ||
					(HereDoc.Quote != '\'' && sc.chPrev == '\\') ||
					(IsBashHereDoc2(sc.ch))) {
					HereDoc.Append(sc.ch);
				} else if (HereDoc.Quoted && sc.ch == HereDoc.Quote) {	// closing quote => end of delimiter
					sc.ForwardSetState(SCE_SH_DEFAULT);
				} else if (sc.ch == '\\') {
					HereDoc.Escaped = true;
					if (HereDoc.Quoted && sc.chNext != HereDoc.Quote && sc.chNext != '\\') {
						// in quoted prefixes only \ and the quote eat the escape
						HereDoc.Append(sc.ch);
					} else {
						// skip escape prefix
					}
				} else if (!HereDoc.Quoted) {
					sc.SetState(SCE_SH_DEFAULT);
				}
				if (HereDoc.DelimiterLength >= HERE_DELIM_MAX - 1) {	// force blowup
					sc.SetState(SCE_SH_ERROR);
					HereDoc.State = 0;
				}
			}
			break;
		case SCE_SH_SCALAR:	// variable names
			if (!IsBashParamChar(sc.ch)) {
				if (sc.LengthCurrent() == 1) {
					// Special variable
					sc.Forward();
				}
				sc.SetState(QuoteStack.State);
				if (QuoteStack.State != SCE_SH_DEFAULT) {
					continue;
				}
			}
			break;
		case SCE_SH_HERE_Q:
			// HereDoc.State == 2
			if (sc.atLineStart/* && QuoteStack.Current.Style == QuoteStyle::HereDoc*/) {
				sc.SetState(SCE_SH_HERE_Q);
				if (HereDoc.Indent) { // tabulation prefix
					while (sc.ch == '\t') {
						sc.Forward();
					}
				}
				int chNext = 0;
				if (HereDoc.DelimiterLength == 0) {
					chNext = sc.ch;
				} else if (styler.Match(sc.currentPos, HereDoc.Delimiter)) {
					chNext = sc.GetRelative(HereDoc.DelimiterLength);
				}
				if (IsEOLChar(chNext) || (chNext == ']' && QuoteStack.dialect == ShellDialect::M4)) {
					if (HereDoc.DelimiterLength != 0) {
						sc.SetState(SCE_SH_HERE_DELIM);
						sc.Forward(HereDoc.DelimiterLength);
					}
					QuoteStack.Pop();
					sc.SetState(SCE_SH_DEFAULT);
					break;
				}
			}
			if (HereDoc.Quoted || HereDoc.Escaped) {
				break;
			}
			// fall through to handle nested shell expansions
			[[fallthrough]];
		case SCE_SH_STRING_DQ:	// delimited styles, can nest
		case SCE_SH_PARAM: // ${parameter}
		case SCE_SH_BACKTICKS:
			if (sc.ch == '\\') {
				QuoteStack.Escape(sc);
			} else if (sc.ch == QuoteStack.Current.Down) {
				if (QuoteStack.CountDown(sc, cmdState)) {
					continue;
				}
			} else if (sc.ch == QuoteStack.Current.Up) {
				if (QuoteStack.Current.Style != QuoteStyle::Parameter) {
					QuoteStack.Current.Count++;
				}
			} else {
				if (QuoteStack.Current.Style == QuoteStyle::String ||
					QuoteStack.Current.Style == QuoteStyle::HereDoc ||
					QuoteStack.Current.Style == QuoteStyle::LString
					) {	// do nesting for "string", $"locale-string", heredoc
					if (sc.ch == '`') {
						QuoteStack.Push(sc.ch, QuoteStyle::Backtick, sc.state, cmdState);
						sc.SetState(SCE_SH_BACKTICKS);
					} else if (sc.ch == '$' && !AnyOf(sc.chNext, '\"', '\'')) {
						QuoteStack.Expand(sc, cmdState);
						continue;
					}
				} else if (QuoteStack.Current.Style == QuoteStyle::Command
					|| QuoteStack.Current.Style == QuoteStyle::Parameter
					|| QuoteStack.Current.Style == QuoteStyle::Backtick
					) {	// do nesting for $(command), `command`, ${parameter}
					if (sc.ch == '\'') {
						if (QuoteStack.dialect == ShellDialect::M4 && QuoteStack.Current.Style == QuoteStyle::Backtick
							&& sc.chPrev > ' ' && !AnyOf(sc.chPrev, '[', '"')) {
							// not command parameter quote, `sed ['']`
							if (QuoteStack.CountDown(sc, cmdState)) {
								continue;
							}
						} else {
							QuoteStack.State = sc.state;
							sc.SetState(SCE_SH_STRING_SQ);
						}
					} else if (sc.ch == '\"') {
						QuoteStack.Push(sc.ch, QuoteStyle::String, sc.state, cmdState);
						sc.SetState(SCE_SH_STRING_DQ);
					} else if (sc.ch == '`') {
						QuoteStack.Push(sc.ch, QuoteStyle::Backtick, sc.state, cmdState);
						sc.SetState(SCE_SH_BACKTICKS);
					} else if (sc.ch == '$') {
						QuoteStack.Expand(sc, cmdState);
						continue;
					}
				}
			}
			break;
		case SCE_SH_STRING_SQ: // singly-quoted strings
			if (sc.ch == '\'') {
				sc.ForwardSetState(QuoteStack.State);
				continue;
			}
			break;
		}

		// Must check end of HereDoc state 1 before default state is handled
		if (HereDoc.State == 1 && sc.MatchLineEnd()) {
			// Begin of here-doc (the line after the here-doc delimiter):
			// Lexically, the here-doc starts from the next line after the >>, but the
			// first line of here-doc seem to follow the style of the last EOL sequence
			HereDoc.State = 2;
			if (HereDoc.Quoted) {
				if (sc.state == SCE_SH_HERE_DELIM) {
					// Missing quote at end of string! Syntax error in bash 4.3
					// Mark this bit as an error, do not colour any here-doc
					sc.ChangeState(SCE_SH_ERROR);
					sc.SetState(SCE_SH_DEFAULT);
				} else {
					// HereDoc.Quote always == '\''
					sc.SetState(SCE_SH_HERE_Q);
					QuoteStack.Start(-1, QuoteStyle::HereDoc, SCE_SH_DEFAULT, cmdState);
				}
			} else if (HereDoc.DelimiterLength == 0) {
				// no delimiter, illegal (but '' and "" are legal)
				sc.ChangeState(SCE_SH_ERROR);
				sc.SetState(SCE_SH_DEFAULT);
			} else {
				sc.SetState(SCE_SH_HERE_Q);
				QuoteStack.Start(-1, QuoteStyle::HereDoc, SCE_SH_DEFAULT, cmdState);
			}
		}

		// update cmdState about the current command segment
		if (stylePrev != SCE_SH_DEFAULT && sc.state == SCE_SH_DEFAULT) {
			cmdState = cmdStateNew;
		}
		// Determine if a new state should be entered.
		if (sc.state == SCE_SH_DEFAULT) {
			if (sc.ch == '\\') {
				// Bash can escape any non-newline as a literal
				sc.SetState(SCE_SH_IDENTIFIER);
				QuoteStack.Escape(sc);
			} else if (IsADigit(sc.ch)) {
				sc.SetState(SCE_SH_NUMBER);
				numBase = 10;
				if (sc.ch == '0' && (sc.chNext == 'x' || sc.chNext == 'X')) {
					numBase = 16;
					sc.Forward();
				} else if (IsADigit(sc.chNext) || sc.chNext == '#') {
					int base = sc.ch - '0';
					if (sc.chNext != '#') {
						base = base*10 + sc.chNext - '0';
						sc.Forward();
					}
					if (sc.chNext == '#' && base >= 2 && base <= 64) {
						numBase = base;
						sc.Forward();
					}
				}
			} else if (IsIdentifierStart(sc.ch)) {
				sc.SetState((cmdState == CmdState::Arithmetic)? SCE_SH_IDENTIFIER : SCE_SH_WORD);
			} else if (sc.ch == '#') {
				if (IsBashMetaCharacter(sc.chPrev)
					|| (sc.chPrev == '[' && QuoteStack.dialect == ShellDialect::M4)) {
					sc.SetState(SCE_SH_COMMENTLINE);
				} else {
					sc.SetState(SCE_SH_WORD);
				}
				// handle some zsh features within arithmetic expressions only
				if (cmdState == CmdState::Arithmetic && QuoteStack.dialect == ShellDialect::Bash) {
					if (sc.chPrev == '[') {	// [#8] [##8] output digit setting
						sc.ChangeState(SCE_SH_WORD);
						if (sc.chNext == '#') {
							sc.Forward();
						}
					} else if (sc.chNext == '#') {
						sc.Forward();
						if (sc.chNext > ' ') { // ##a
							sc.ChangeState(SCE_SH_IDENTIFIER);
							if (sc.chNext == '^' && IsUpperCase(sc.GetRelative(2))) { // ##^A
								sc.Forward();
							}
						}
					} else if (IsIdentifierStart(sc.chNext)) {	// #name
						sc.ChangeState(SCE_SH_IDENTIFIER);
					}
				}
			} else if (sc.ch == '\"') {
				QuoteStack.Start(sc.ch, QuoteStyle::String, SCE_SH_DEFAULT, cmdState);
				sc.SetState(SCE_SH_STRING_DQ);
			} else if (sc.ch == '\'') {
				if (QuoteStack.dialect == ShellDialect::M4 && stylePrev != SCE_SH_SCALAR && IsAlphaNumeric(sc.chPrev)) {
					// treated as apostrophe: one's, don't
				} else {
					QuoteStack.State = SCE_SH_DEFAULT;
					sc.SetState(SCE_SH_STRING_SQ);
				}
			} else if (sc.ch == '`') {
				QuoteStack.Start(sc.ch, QuoteStyle::Backtick, SCE_SH_DEFAULT, cmdState);
				sc.SetState(SCE_SH_BACKTICKS);
			} else if (sc.ch == '$') {
				QuoteStack.Expand(sc, cmdState);
				continue;
			} else if (cmdState != CmdState::Arithmetic && sc.Match('<', '<')) {
				sc.SetState(SCE_SH_HERE_DELIM);
				HereDoc.State = 0;
				HereDoc.Indent = false;
				if (sc.GetRelative(2) == '-') {	// <<- indent case
					HereDoc.Indent = true;
					sc.Forward();
				}
			} else if (sc.ch == '-' && // test operator or short and long option
				cmdState != CmdState::Arithmetic &&
				sc.chPrev != '~' && !IsADigit(sc.chNext)) {
				sc.SetState(IsBashMetaCharacter(sc.chPrev) ? SCE_SH_WORD : SCE_SH_IDENTIFIER);
			} else if (IsAGraphic(sc.ch) && (sc.ch != '/' || cmdState == CmdState::Arithmetic)) {
				sc.SetState(SCE_SH_OPERATOR);
				// arithmetic expansion and command substitution
				if (QuoteStack.Current.Style >= QuoteStyle::Command) {
					if (sc.ch == QuoteStack.Current.Down) {
						if (QuoteStack.CountDown(sc, cmdState)) {
							continue;
						}
					} else if (sc.ch == QuoteStack.Current.Up) {
						QuoteStack.Current.Count++;
					}
				}
				// globs have no whitespace, do not appear in arithmetic expressions
				if (cmdState != CmdState::Arithmetic && sc.ch == '(' && sc.chNext != '(') {
					const int i = GlobScan(sc);
					if (i > 1) {
						sc.SetState(SCE_SH_IDENTIFIER);
						sc.Forward(i + 1);
						continue;
					}
				}
				// handle opening delimiters for test/arithmetic expressions - ((,[[,[
				if (cmdState <= CmdState::Start) {
					if (sc.Match('(', '(')) {
						cmdState = CmdState::Arithmetic;
						sc.Forward();
					} else if (sc.ch == '[') {
						if (sc.chNext == '[') {
							sc.Forward();
						}
						if (IsASpace(sc.chNext)) {
							cmdState = (sc.chPrev == '[') ? CmdState::DoubleBracket : CmdState::SingleBracket;
						} else if (QuoteStack.dialect == ShellDialect::M4) {
							cmdState = CmdState::Delimiter;
						}
					}
				}
				// special state -- for ((x;y;z)) in ... looping
				if (cmdState == CmdState::Word && sc.Match('(', '(')) {
					cmdState = CmdState::Arithmetic;
					sc.Forward();
				}
				// handle command delimiters in command Start|Body|Word state, also Test if 'test' or '[]'
				if (cmdState < CmdState::DoubleBracket) {
					if (IsBashCmdDelimiter(sc.ch, sc.chNext)) {
						cmdState = CmdState::Delimiter;
						sc.Forward();
					} else if (IsBashCmdDelimiter(sc.ch)) {
						cmdState = CmdState::Delimiter;
					}
				}
				// handle closing delimiters for test/arithmetic expressions - )),]],]
				if (cmdState == CmdState::Arithmetic && sc.Match(')', ')')) {
					cmdState = CmdState::Body;
					sc.Forward();
				} else if (sc.ch == ']' && IsASpace(sc.chPrev)) {
					if (cmdState == CmdState::SingleBracket) {
						cmdState = CmdState::Body;
					} else if (cmdState == CmdState::DoubleBracket && sc.chNext == ']') {
						cmdState = CmdState::Body;
						sc.Forward();
					}
				}
			}
		}// sc.state

		sc.Forward();
	}
	if (sc.state == SCE_SH_HERE_Q) {
		styler.ChangeLexerState(sc.currentPos, styler.Length());
	}
	sc.Complete();
}

#define IsCommentLine(line)	IsLexCommentLine(styler, line, SCE_SH_COMMENTLINE)

void FoldBashDoc(Sci_PositionU startPos, Sci_Position length, int initStyle, LexerWordList /*keywordLists*/, Accessor &styler) {
	const ShellDialect dialect = static_cast<ShellDialect>(styler.GetPropertyInt("lexer.lang"));

	const Sci_PositionU endPos = startPos + length;
	Sci_Line lineCurrent = styler.GetLine(startPos);
	Sci_PositionU lineStartNext = styler.LineStart(lineCurrent + 1);
	lineStartNext = sci::min(lineStartNext, endPos);
	int levelPrev = styler.LevelAt(lineCurrent) & SC_FOLDLEVELNUMBERMASK;
	int levelCurrent = levelPrev;
	int styleNext = styler.StyleAt(startPos);
	int style = initStyle;

	char word[8]; // foreach
	constexpr int MaxFoldWordLength = sizeof(word) - 1;
	int wordlen = 0;

	while (startPos < endPos) {
		const char ch = styler[startPos];
		const int stylePrev = style;
		style = styleNext;
		styleNext = styler.StyleAt(++startPos);

		switch (style) {
		case SCE_SH_WORD:
			if (wordlen < MaxFoldWordLength) {
				word[wordlen++] = ch;
			}
			if (styleNext != SCE_SH_WORD) {
				word[wordlen] = '\0';
				wordlen = 0;
				if (dialect == ShellDialect::CShell) {
					if (StrEqualsAny(word, "if", "foreach", "switch", "while")) {
						levelCurrent++;
					} else if (StrEqualsAny(word, "end", "endif", "endsw")) {
						levelCurrent--;
					}
				} else {
					if (StrEqualsAny(word, "if", "case", "do")) {
						levelCurrent++;
					} else if (StrEqualsAny(word, "fi", "esac", "done")) {
						levelCurrent--;
					}
				}
			}
			break;

		case SCE_SH_OPERATOR:
			if (AnyOf<'{', '}'>(ch)) {
				levelCurrent += ('{' + '}')/2 - ch;
			} else if (dialect == ShellDialect::M4 && AnyOf<'[', ']'>(ch)) {
				// TODO: fix unmatched bracket in glob pattern
				levelCurrent += ('[' + ']')/2 - ch;
			}
			break;

		case SCE_SH_HERE_DELIM:
			if (stylePrev == SCE_SH_HERE_Q) {
				levelCurrent--;
			} else if (stylePrev != SCE_SH_HERE_DELIM) {
				if (ch == '<' && styler[startPos + 1] != '<') {
					levelCurrent++;
				}
			}
			break;

		case SCE_SH_HERE_Q:
			if (styleNext == SCE_SH_DEFAULT) {
				levelCurrent--;
			}
			break;
		}

		if (startPos == lineStartNext) {
			levelCurrent = sci::max(levelCurrent, SC_FOLDLEVELBASE);
			// Comment folding
			if (IsCommentLine(lineCurrent)) {
				levelCurrent += IsCommentLine(lineCurrent + 1) - IsCommentLine(lineCurrent - 1);
			}

			int lev = levelPrev;
			if ((levelCurrent > levelPrev)) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			}
			styler.SetLevel(lineCurrent, lev);
			lineCurrent++;
			lineStartNext = styler.LineStart(lineCurrent + 1);
			lineStartNext = sci::min(lineStartNext, endPos);
			levelPrev = levelCurrent;
		}
	}
}

}

LexerModule lmBash(SCLEX_BASH, ColouriseBashDoc, "bash", FoldBashDoc);
