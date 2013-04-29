/*
*******************************************************************************
* Copyright (C) 2013, International Business Machines
* Corporation and others.  All Rights Reserved.
*******************************************************************************
* collationruleparser.cpp
*
* created on: 2013apr10
* created by: Markus W. Scherer
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_COLLATION

#include "unicode/normalizer2.h"
#include "unicode/parseerr.h"
#include "unicode/uchar.h"
#include "unicode/ucol.h"
#include "unicode/unistr.h"
#include "unicode/utf16.h"
#include "charstr.h"
#include "cmemory.h"
#include "collation.h"
#include "collationdata.h"
#include "collationruleparser.h"
#include "collationsettings.h"
#include "cstring.h"
#include "patternprops.h"
#include "uassert.h"

#define LENGTHOF(array) (int32_t)(sizeof(array)/sizeof((array)[0]))

U_NAMESPACE_BEGIN

namespace {

static const UChar BEFORE[] = { 0x5b, 0x62, 0x65, 0x66, 0x6f, 0x72, 0x65, 0 };  // "[before"
const int32_t BEFORE_LENGTH = 7;

}  // namespace

CollationRuleParser::Sink::~Sink() {}

CollationRuleParser::Importer::~Importer() {}

CollationRuleParser::CollationRuleParser(UErrorCode &errorCode)
        : nfd(*Normalizer2::getNFDInstance(errorCode)),
          fcc(*Normalizer2::getInstance(NULL, "nfc", UNORM2_COMPOSE_CONTIGUOUS, errorCode)),
          rules(NULL), settings(NULL),
          parseError(NULL), errorReason(NULL),
          sink(NULL), importer(NULL),
          ruleIndex(0) {
}

CollationRuleParser::~CollationRuleParser() {
}

void
CollationRuleParser::parse(const UnicodeString &ruleString,
                           const CollationData *base,
                           CollationSettings &outSettings,
                           UParseError *outParseError,
                           UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    baseData = base;
    settings = &outSettings;
    parseError = outParseError;
    if(parseError != NULL) {
        parseError->line = 0;
        parseError->offset = 0;
        parseError->preContext[0] = 0;
        parseError->postContext[0] = 0;
    }
    errorReason = NULL;
    parse(ruleString, errorCode);
}

void
CollationRuleParser::parse(const UnicodeString &ruleString, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    rules = &ruleString;
    ruleIndex = 0;

    while(ruleIndex < rules->length()) {
        UChar c = rules->charAt(ruleIndex);
        if(PatternProps::isWhiteSpace(c)) {
            ++ruleIndex;
            continue;
        }
        switch(c) {
        case 0x26:  // '&'
            parseRuleChain(errorCode);
            break;
        case 0x5b:  // '['
            parseSetting(errorCode);
            break;
        case 0x23:  // '#' starts a comment, until the end of the line
            ruleIndex = skipComment(ruleIndex + 1);
            break;
        case 0x40:  // '@' is equivalent to [backwards 2]
            settings->setFlag(CollationSettings::BACKWARD_SECONDARY,
                              UCOL_ON, 0, errorCode);
            ++ruleIndex;
            break;
        case 0x21:  // '!' used to turn on Thai/Lao character reversal
            // Accept but ignore. The root collator has contractions
            // that are equivalent to the character reversal, where appropriate.
            ++ruleIndex;
            break;
        default:
            setParseError("expected a reset or setting or comment", errorCode);
            break;
        }
        if(U_FAILURE(errorCode)) { return; }
    }
}

void
CollationRuleParser::parseRuleChain(UErrorCode &errorCode) {
    int32_t resetStrength = parseResetAndPosition(errorCode);
    UBool isFirstRelation = TRUE;
    for(;;) {
        int32_t result = parseRelationOperator(errorCode);
        if(U_FAILURE(errorCode)) { return; }
        if(result == NO_RELATION) {
            if(ruleIndex < rules->length() && rules->charAt(ruleIndex) == 0x23) {
                // '#' starts a comment, until the end of the line
                ruleIndex = skipWhiteSpace(skipComment(ruleIndex + 1));
                continue;
            }
            if(isFirstRelation) {
                setParseError("reset not followed by a relation", errorCode);
            }
            return;
        }
        int32_t strength = result & STRENGTH_MASK;
        if(resetStrength < IDENTICAL) {
            // reset-before rule chain
            if(isFirstRelation) {
                if(strength != resetStrength) {
                    setParseError("reset-before strength differs from its first relation", errorCode);
                    return;
                }
            } else {
                if(strength < resetStrength) {
                    setParseError("reset-before strength followed by a stronger relation", errorCode);
                    return;
                }
            }
        }
        int32_t i = ruleIndex + (result >> 8);  // skip over the relation operator
        if((result & 0x10) == 0) {
            parseRelationStrings(strength, i, errorCode);
        } else {
            parseStarredCharacters(strength, i, errorCode);
        }
        if(U_FAILURE(errorCode)) { return; }
        isFirstRelation = FALSE;
    }
}

int32_t
CollationRuleParser::parseResetAndPosition(UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return NO_RELATION; }
    int32_t i = skipWhiteSpace(ruleIndex + 1);
    int32_t j;
    UChar c;
    int32_t resetStrength;
    if(rules->compare(i, BEFORE_LENGTH, BEFORE, 0, BEFORE_LENGTH) == 0 &&
            (j = i + BEFORE_LENGTH) < rules->length() &&
            PatternProps::isWhiteSpace(rules->charAt(j)) &&
            ((j = skipWhiteSpace(j + 1)) + 1) < rules->length() &&
            0x31 <= (c = rules->charAt(j)) && c <= 0x33 &&
            rules->charAt(j + 1) == 0x5d) {
        // &[before n] with n=1 or 2 or 3
        resetStrength = PRIMARY + (c - 0x31);
        i = skipWhiteSpace(j + 2);
    } else {
        resetStrength = IDENTICAL;
    }
    if(i >= rules->length()) {
        setParseError("reset without position", errorCode);
        return NO_RELATION;
    }
    resetTailoringStrings();
    if(rules->charAt(i) == 0x5b) {  // '['
        i = parseSpecialPosition(i, errorCode);
    } else {
        i = parseTailoringString(i, errorCode);
        fcc.normalize(raw, str, errorCode);
    }
    sink->addReset(resetStrength, str, errorReason, errorCode);
    ruleIndex = i;
    return resetStrength;
}

int32_t
CollationRuleParser::parseRelationOperator(UErrorCode &errorCode) {
    if(U_FAILURE(errorCode) || ruleIndex >= rules->length()) { return NO_RELATION; }
    int32_t strength;
    int32_t i = ruleIndex;
    UChar c = rules->charAt(i++);
    switch(c) {
    case 0x3c:  // '<'
        if(i < rules->length() && rules->charAt(i) == 0x3c) {  // <<
            ++i;
            if(i < rules->length() && rules->charAt(i) == 0x3c) {  // <<<
                ++i;
                strength = TERTIARY;
            } else {
                strength = SECONDARY;
            }
        } else {
            strength = PRIMARY;
        }
        if(i < rules->length() && rules->charAt(i) == 0x2a) {  // '*'
            ++i;
            strength |= 0x10;
        }
        break;
    case 0x3b:  // ';' same as <<
        strength = SECONDARY;
        break;
    case 0x2c:  // ',' same as <<<
        strength = TERTIARY;
        break;
    case 0x3d:  // '='
        strength = IDENTICAL;
        if(i < rules->length() && rules->charAt(i) == 0x2a) {  // '*'
            ++i;
            strength |= 0x10;
        }
        break;
    default:
        return NO_RELATION;
    }
    return ((i - ruleIndex) << 8) | strength;
}

void
CollationRuleParser::parseRelationStrings(int32_t strength, int32_t i, UErrorCode &errorCode) {
    // Parse
    //     prefix | str / extension
    // where prefix and extension are optional.
    resetTailoringStrings();
    i = parseTailoringString(i, errorCode);
    if(U_FAILURE(errorCode)) { return; }
    UChar next = (i < rules->length()) ? rules->charAt(i) : 0;
    if(next == 0x7c) {  // '|' separates the context prefix from the string.
        fcc.normalize(raw, prefix, errorCode);
        i = parseTailoringString(i + 1, errorCode);
        if(U_FAILURE(errorCode)) { return; }
        next = (i < rules->length()) ? rules->charAt(i) : 0;
    }
    fcc.normalize(raw, str, errorCode);
    if(next == 0x2f) {  // '/' separates the string from the extension.
        i = parseTailoringString(i + 1, errorCode);
        fcc.normalize(raw, extension, errorCode);
    }
    // TODO: if(!prefix.isEmpty()) { check that prefix and str start with hasBoundaryBefore }
    sink->addRelation(strength, prefix, str, extension, errorReason, errorCode);
    ruleIndex = i;
}

void
CollationRuleParser::parseStarredCharacters(int32_t strength, int32_t i, UErrorCode &errorCode) {
    resetTailoringStrings();
    i = parseString(i, TRUE, errorCode);
    if(U_FAILURE(errorCode)) { return; }
    UChar32 prev = -1;
    for(int32_t j = 0; j < raw.length() && U_SUCCESS(errorCode);) {
        UChar32 c = raw.char32At(j);
        if(c != 0x2d) {  // '-'
            if(!nfd.isInert(c)) {
                setParseError("starred-relation string is not all NFD-inert", errorCode);
                return;
            }
            str.setTo(c);
            sink->addRelation(strength, prefix, str, extension, errorReason, errorCode);
            j += U16_LENGTH(c);
            prev = c;
        } else {
            if(prev < 0) {
                setParseError("range without start in starred-relation string", errorCode);
                return;
            }
            if(++j == raw.length()) {
                setParseError("range without end in starred-relation string", errorCode);
                return;
            }
            c = raw.char32At(j);
            if(!nfd.isInert(c)) {
                setParseError("starred-relation string is not all NFD-inert", errorCode);
                return;
            }
            if(c < prev) {
                setParseError("range start greater than end in starred-relation string", errorCode);
                return;
            }
            j += U16_LENGTH(c);
            // range prev-c
            while(++prev <= c) {
                str.setTo(prev);
                sink->addRelation(strength, prefix, str, extension, errorReason, errorCode);
            }
            prev = -1;
        }
    }
    ruleIndex = i;
}

int32_t
CollationRuleParser::parseTailoringString(int32_t i, UErrorCode &errorCode) {
    i = parseString(i, FALSE, errorCode);
    int32_t nfdLength = nfd.normalize(raw, errorCode).length();
    if(nfdLength > 31) {  // Limited by token string encoding.
        setParseError("tailoring string too long", errorCode);
    }
    return i;
}

int32_t
CollationRuleParser::parseString(int32_t i, UBool allowDash, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return i; }
    raw.remove();
    for(i = skipWhiteSpace(i); i < rules->length();) {
        UChar32 c = rules->charAt(i++);
        if(isSyntaxChar(c)) {
            if(c == 0x27) {  // apostrophe
                if(i < rules->length() && rules->charAt(i) == 0x27) {
                    // Double apostrophe, encodes a single one.
                    raw.append((UChar)0x27);
                    ++i;
                    continue;
                }
                // Quote literal text until the next single apostrophe.
                for(;;) {
                    if(i == rules->length()) {
                        setParseError("quoted literal text missing terminating apostrophe", errorCode);
                        return i;
                    }
                    c = rules->charAt(i++);
                    if(c == 0x27) {
                        if(i < rules->length() && rules->charAt(i) == 0x27) {
                            // Double apostrophe inside quoted literal text,
                            // still encodes a single apostrophe.
                            ++i;
                        } else {
                            break;
                        }
                    }
                    raw.append((UChar)c);
                }
            } else if(c == 0x5c) {  // backslash
                if(i == rules->length()) {
                    setParseError("backslash escape at the end of the rule string", errorCode);
                    return i;
                }
                c = rules->char32At(i);
                raw.append(c);
                i += U16_LENGTH(c);
            } else if(c == 0x2d && allowDash) {  // '-'
                raw.append((UChar)c);
            } else {
                // Any other syntax character terminates a string.
                break;
            }
        } else if(PatternProps::isWhiteSpace(c)) {
            // Unquoted white space terminates a string.
            i = skipWhiteSpace(i);
            break;
        } else {
            raw.append((UChar)c);
        }
    }
    if(raw.isEmpty()) {
        setParseError("missing string", errorCode);
        return i;
    }
    for(int32_t j = 0; j < raw.length();) {
        UChar32 c = raw.char32At(j);
        if(U_IS_SURROGATE(c)) {
            setParseError("string contains an unpaired surrogate", errorCode);
            return i;
        }
        if(c == 0xfffe || c == 0xffff) {
            setParseError("string contains U+FFFE or U+FFFF", errorCode);
            return i;
        }
        j += U16_LENGTH(c);
    }
    return i;
}

namespace {

static const char *const positions[] = {
    "first tertiary ignorable",
    "last tertiary ignorable",
    "first secondary ignorable",
    "last secondary ignorable",
    "first primary ignorable",
    "last primary ignorable",
    "first variable",
    "last variable",
    "first implicit",
    "last implicit",
    "first regular",
    "last regular",
    "first trailing",
    "last trailing"
};

}  // namespace

int32_t
CollationRuleParser::parseSpecialPosition(int32_t i, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return 0; }
    int32_t j = readWords(i);
    if(j > i && rules->charAt(j) == 0x5d && !raw.isEmpty()) {  // words end with ]
        ++j;
        for(int32_t pos = 0; pos < LENGTHOF(positions); ++pos) {
            if(raw == UnicodeString(positions[pos], -1, US_INV)) {
                str.setTo((UChar)POS_LEAD).append((UChar)(POS_BASE + pos));
                return j;
            }
        }
        if(raw == UNICODE_STRING_SIMPLE("top")) {
            str.setTo((UChar)POS_LEAD).append((UChar)(POS_BASE + LAST_REGULAR));
            return j;
        }
        if(raw == UNICODE_STRING_SIMPLE("variable top")) {
            str.setTo((UChar)POS_LEAD).append((UChar)(POS_BASE + LAST_VARIABLE));
            return j;
        }
    }
    setParseError("not a valid special reset position", errorCode);
    return i;
}

void
CollationRuleParser::parseSetting(UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    int32_t i = ruleIndex + 1;
    int32_t j = readWords(i);
    if(j <= i || raw.isEmpty()) {
        setParseError("expected a setting/option at '['", errorCode);
    }
    if(rules->charAt(j) == 0x5d) {  // words end with ]
        ++j;
        if(raw.startsWith(UNICODE_STRING_SIMPLE("reorder")) &&
                (raw.length() == 7 || raw.charAt(7) == 0x20)) {
            parseReordering(errorCode);
            ruleIndex = j;
            return;
        }
        if(raw == UNICODE_STRING_SIMPLE("backwards 2")) {
            settings->setFlag(CollationSettings::BACKWARD_SECONDARY,
                              UCOL_ON, 0, errorCode);
            ruleIndex = j;
            return;
        }
        UnicodeString v;
        int32_t valueIndex = raw.lastIndexOf((UChar)0x20);
        if(valueIndex >= 0) {
            v.setTo(raw, valueIndex + 1);
            raw.truncate(valueIndex);
        }
        if(raw == UNICODE_STRING_SIMPLE("strength") && v.length() == 1) {
            int32_t value = UCOL_DEFAULT;
            UChar c = v.charAt(0);
            if(0x31 <= c && c <= 0x34) {  // 1..4
                value = UCOL_PRIMARY + (c - 0x31);
            } else if(c == 0x49) {  // 'I'
                value = UCOL_IDENTICAL;
            }
            if(value != UCOL_DEFAULT) {
                settings->setStrength(value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("alternate")) {
            UColAttributeValue value = UCOL_DEFAULT;
            if(v == UNICODE_STRING_SIMPLE("non-ignorable")) {
                value = UCOL_NON_IGNORABLE;
            } else if(v == UNICODE_STRING_SIMPLE("shifted")) {
                value = UCOL_SHIFTED;
            }
            if(value != UCOL_DEFAULT) {
                settings->setAlternateHandling(value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("caseFirst")) {
            UColAttributeValue value = UCOL_DEFAULT;
            if(v == UNICODE_STRING_SIMPLE("off")) {
                value = UCOL_OFF;
            } else if(v == UNICODE_STRING_SIMPLE("lower")) {
                value = UCOL_LOWER_FIRST;
            } else if(v == UNICODE_STRING_SIMPLE("upper")) {
                value = UCOL_UPPER_FIRST;
            }
            if(value != UCOL_DEFAULT) {
                settings->setCaseFirst(value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("caseLevel")) {
            UColAttributeValue value = getOnOffValue(v);
            if(value != UCOL_DEFAULT) {
                settings->setFlag(CollationSettings::CASE_LEVEL, value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("normalization")) {
            UColAttributeValue value = getOnOffValue(v);
            if(value != UCOL_DEFAULT) {
                settings->setFlag(CollationSettings::CHECK_FCD, value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("numericOrdering")) {
            UColAttributeValue value = getOnOffValue(v);
            if(value != UCOL_DEFAULT) {
                settings->setFlag(CollationSettings::NUMERIC, value, 0, errorCode);
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("hiraganaQ")) {
            UColAttributeValue value = getOnOffValue(v);
            if(value != UCOL_DEFAULT) {
                if(value == UCOL_ON) {
                    setParseError("[hiraganaQ on] is not supported", errorCode);  // TODO
                }
                // TODO
                ruleIndex = j;
                return;
            }
        } else if(raw == UNICODE_STRING_SIMPLE("import")) {
            CharString lang;
            lang.appendInvariantChars(v, errorCode);
            if(errorCode == U_MEMORY_ALLOCATION_ERROR) { return; }
            char localeID[ULOC_FULLNAME_CAPACITY];
            int32_t parsedLength;
            int32_t length = uloc_forLanguageTag(lang.data(), localeID, ULOC_FULLNAME_CAPACITY,
                                                 &parsedLength, &errorCode);
            if(U_FAILURE(errorCode) ||
                    parsedLength != lang.length() || length >= ULOC_FULLNAME_CAPACITY) {
                errorCode = U_ZERO_ERROR;
                setParseError("expected language tag in [import langTag]", errorCode);
                return;
            }
            // TODO: Split locale vs. collation keyword value like in ucol_tok.cpp
            // before calling importer?
            if(importer == NULL) {
                setParseError("[import langTag] is not supported", errorCode);
            } else {
                const UnicodeString *importedRules =
                    importer->getRules(localeID, errorReason, errorCode);
                parse(*importedRules, errorCode);
                ruleIndex = j;
            }
            return;
        }
    } else if(rules->charAt(j) == 0x5b) {  // words end with [
        UnicodeSet set;
        j = parseUnicodeSet(j, set, errorCode);
        if(raw == UNICODE_STRING_SIMPLE("optimize")) {
            optimizeSet.addAll(set);
            ruleIndex = j;
            return;
        } else if(raw == UNICODE_STRING_SIMPLE("suppressContractions")) {
            sink->suppressContractions(set, errorReason, errorCode);
            ruleIndex = j;
            return;
        }
    }
    setParseError("not a valid setting/option", errorCode);
}

void
CollationRuleParser::parseReordering(UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    uprv_free(const_cast<int32_t *>(settings->reorderCodes));
    settings->reorderCodes = NULL;
    settings->reorderCodesLength = 0;
    int32_t i = 7;  // after "reorder"
    if(i == raw.length()) {
        // empty [reorder] with no codes
        uprv_free(const_cast<uint8_t *>(settings->reorderTable));
        settings->reorderTable = NULL;
        return;
    }
    // Count the codes in [reorder aa bb cc], same as the number of collapsed spaces.
    int32_t length = 0;
    for(int32_t j = i; j < raw.length(); ++j) {
        if(raw.charAt(j) == 0x20) { ++length; }
    }
    int32_t *newReorderCodes = (int32_t *)uprv_malloc(length * 4);
    if(newReorderCodes == NULL) {
        errorCode = U_MEMORY_ALLOCATION_ERROR;
        return;
    }
    settings->reorderCodes = newReorderCodes;
    int32_t codeIndex = 0;
    CharString word;
    while(i < raw.length()) {
        ++i;  // skip the word-separating space
        int32_t limit = raw.indexOf((UChar)0x20, i);
        if(limit < 0) { limit = raw.length(); }
        word.clear().appendInvariantChars(raw.tempSubStringBetween(i, limit), errorCode);
        if(U_FAILURE(errorCode)) { return; }
        int32_t code = getReorderCode(word.data());
        if(code < 0) {
            setParseError("unknown script or reorder code", errorCode);
            return;
        }
        newReorderCodes[codeIndex++] = code;
        i = limit;
    }
    U_ASSERT(codeIndex == length);
    uint8_t *reorderTable = const_cast<uint8_t *>(settings->reorderTable);
    if(length == 1 && newReorderCodes[0] == UCOL_REORDER_CODE_DEFAULT) {
        // The root collator does not have a reordering, by definition.
        uprv_free(newReorderCodes);
        settings->reorderCodes = NULL;
        settings->reorderCodesLength = 0;
        uprv_free(reorderTable);
        settings->reorderTable = NULL;
        return;
    }
    if(reorderTable == NULL) {
        reorderTable = (uint8_t *)uprv_malloc(256);
        if(reorderTable == NULL) {
            errorCode = U_MEMORY_ALLOCATION_ERROR;
            return;
        }
        settings->reorderTable = reorderTable;
    }
    baseData->makeReorderTable(settings->reorderCodes, length, reorderTable, errorCode);
    settings->reorderCodesLength = length;
}

static const char *const gSpecialReorderCodes[] = {
    "space", "punct", "symbol", "currency", "digit"
};

int32_t
CollationRuleParser::getReorderCode(const char *word) {
    for(int32_t i = 0; i < LENGTHOF(gSpecialReorderCodes); ++i) {
        if(uprv_stricmp(word, gSpecialReorderCodes[i]) == 0) {
            return UCOL_REORDER_CODE_FIRST + i;
        }
    }
    return u_getPropertyValueEnum(UCHAR_SCRIPT, word);
}

UColAttributeValue
CollationRuleParser::getOnOffValue(const UnicodeString &s) {
    if(s == UNICODE_STRING_SIMPLE("on")) {
        return UCOL_ON;
    } else if(s == UNICODE_STRING_SIMPLE("off")) {
        return UCOL_OFF;
    } else {
        return UCOL_DEFAULT;
    }
}

int32_t
CollationRuleParser::parseUnicodeSet(int32_t i, UnicodeSet &set, UErrorCode &errorCode) {
    // Collect a UnicodeSet pattern between a balanced pair of [brackets].
    int32_t level = 0;
    int32_t j = i;
    for(;;) {
        if(j == raw.length()) {
            setParseError("unbalanced UnicodeSet pattern brackets", errorCode);
            return j;
        }
        UChar c = raw.charAt(j++);
        if(c == 0x5b) {  // '['
            ++level;
        } else if(c == 0x5c) {  // ']'
            if(--level == 0) { break; }
        }
    }
    set.applyPattern(raw.tempSubStringBetween(i, j), errorCode);
    if(U_FAILURE(errorCode)) {
        errorCode = U_ZERO_ERROR;
        setParseError("not a valid UnicodeSet pattern", errorCode);
        return j;
    }
    j = skipWhiteSpace(j);
    if(j == raw.length() || raw.charAt(j) != 0x5d) {
        setParseError("missing option-terminating ']' after UnicodeSet pattern", errorCode);
        return j;
    }
    return ++j;
}

int32_t
CollationRuleParser::readWords(int32_t i) {
    static const UChar sp = 0x20;
    raw.remove();
    i = skipWhiteSpace(i);
    for(;;) {
        if(i >= rules->length()) { return 0; }
        UChar c = rules->charAt(i);
        if(isSyntaxChar(c) && c != 0x2d && c != 0x5f) {  // syntax except -_
            if(raw.isEmpty()) { return i; }
            if(raw.endsWith(&sp, 1)) {  // remove trailing space
                raw.truncate(raw.length() - 1);
            }
            return i;
        }
        if(PatternProps::isWhiteSpace(c)) {
            raw.append(0x20);
            i = skipWhiteSpace(i + 1);
        } else {
            raw.append(c);
            ++i;
        }
    }
}

int32_t
CollationRuleParser::skipComment(int32_t i) const {
    // skip to past the newline
    while(i < rules->length()) {
        UChar c = rules->charAt(i++);
        // LF or FF or CR or NEL or LS or PS
        if(c == 0xa || c == 0xc || c == 0xd || c == 0x85 || c == 0x2028 || c == 0x2029) {
            // Unicode Newline Guidelines: "A readline function should stop at NLF, LS, FF, or PS."
            // NLF (new line function) = CR or LF or CR+LF or NEL.
            // No need to collect all of CR+LF because a following LF will be ignored anyway.
            break;
        }
    }
    return i;
}

#if 0
// TODO: move to Sink (CollationBuilder)
void
CollationRuleParser::makeAndInsertToken(int32_t relation, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    U_ASSERT(NO_RELATION < (relation & STRENGTH_MASK) && (relation & STRENGTH_MASK) <= IDENTICAL);
    U_ASSERT((relation & ~RELATION_MASK) == 0);
    U_ASSERT(!str.isEmpty());
    // Find the tailoring in a previous rule chain.
    // Merge the new relation with any existing one for prefix+str.
    // If new, then encode the relation and tailoring strings into a token word,
    // and insert the token at tokenIndex.
    int32_t token = relation;
    if(!prefix.isEmpty()) {
        token |= HAS_PREFIX;
    }
    if(str.hasMoreChar32Than(0, 0x7fffffff, 1)) {
        token |= HAS_CONTRACTION;
    }
    if(!extension.isEmpty()) {
        token |= HAS_EXTENSION;
    }
    UChar32 c = str.char32At(0);
    if((token & HAS_STRINGS) == 0) {
        if(tailoredSet.contains(c)) {
            // TODO: search for existing token
        } else {
            tailoredSet.add(c);
        }
        token |= c << VALUE_SHIFT;
    } else {
        // The relation maps from str if preceded by the prefix. Look for a duplicate.
        // We ignore the extension because that adds to the CEs for the relation
        // but is not part of what is being tailored (what is mapped *from*).
        UnicodeString s(prefix);
        s.append(str);
        int32_t lengthsWord = prefix.length() | (str.length() << 5);
        if((token & HAS_CONTEXT) == 0 ? tailoredSet.contains(c) : tailoredSet.contains(s)) {
            // TODO: search for existing token
        } else if((token & HAS_CONTEXT) == 0) {
            tailoredSet.add(c);
        } else {
            tailoredSet.add(s);
        }
        int32_t value = tokenStrings.length();
        if(value > MAX_VALUE) {
            setParseError("total tailoring strings overflow", errorCode);
            return;
        }
        token |= ~value << VALUE_SHIFT;  // negative
        lengthsWord |= (extension.length() << 10);
        tokenStrings.append((UChar)lengthsWord).append(s).append(extension);
    }
    if((relation & DIFF) != 0) {
        // Postpone insertion:
        // Insert the new relation before the next token with a relation at least as strong.
        // Stops before resets and the sentinel.
        for(;;) {
            int32_t t = tokens.elementAti(tokenIndex);
            if((t & RELATION_MASK) <= relation) { break; }
            ++tokenIndex;
        }
    } else {
        // Append a new reset at the end of the token list,
        // before the sentinel, starting a new rule chain.
        tokenIndex = tokens.size() - 1;
    }
    tokens.insertElementAt(token, tokenIndex++, errorCode);
}
#endif

void
CollationRuleParser::resetTailoringStrings() {
    prefix.remove();
    str.remove();
    extension.remove();
}

void
CollationRuleParser::setParseError(const char *reason, UErrorCode &errorCode) {
    if(U_FAILURE(errorCode)) { return; }
    errorCode = U_PARSE_ERROR;
    errorReason = reason;
    if(parseError == NULL) { return; }

    // Note: This relies on the calling code maintaining the ruleIndex
    // at a position that is useful for debugging.
    // For example, at the beginning of a reset or relation etc.
    parseError->offset = ruleIndex;
    parseError->line = 0;  // We are not counting line numbers.

    // before ruleIndex
    int32_t start = ruleIndex - (U_PARSE_CONTEXT_LEN - 1);
    if(start < 0) {
        start = 0;
    } else if(start > 0 && U16_IS_TRAIL(rules->charAt(start))) {
        ++start;
    }
    int32_t length = ruleIndex - start;
    rules->extract(start, length, parseError->preContext);
    parseError->preContext[length] = 0;

    // starting from ruleIndex
    length = rules->length() - ruleIndex;
    if(length >= U_PARSE_CONTEXT_LEN) {
        length = U_PARSE_CONTEXT_LEN - 1;
        if(U16_IS_LEAD(rules->charAt(ruleIndex + length - 1))) {
            --length;
        }
    }
    rules->extract(ruleIndex, length, parseError->postContext);
    parseError->postContext[length] = 0;
}

UBool
CollationRuleParser::isSyntaxChar(UChar32 c) {
    return 0x21 <= c && c <= 0x7e &&
            (c <= 0x2f || (0x3a <= c && c <= 0x40) ||
            (0x5b <= c && c <= 0x60) || (0x7b <= c));
}

int32_t
CollationRuleParser::skipWhiteSpace(int32_t i) const {
    while(i < rules->length() && PatternProps::isWhiteSpace(rules->charAt(i))) {
        ++i;
    }
    return i;
}

U_NAMESPACE_END

#endif  // !UCONFIG_NO_COLLATION
