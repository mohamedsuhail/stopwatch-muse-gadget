/*
 * Muse's text as something to say aloud: the same cleanup as speakable() in the
 * patch's muse_chat_session.cpp. List numbers ("1." "2)") and bullets ("-" "*"
 * "#" "•") at the start of a line go, markdown marks and emoji go, and dashes
 * become short pauses.
 */
export function speakable(text) {
    return text
        .split('\n')
        .map((line) => line.replace(/^ *(\d+[.)] |[-*#] |• )/, ''))
        .join('\n')
        .replace(/[*_`#]/g, '')
        .replace(/ *[–—] */g, ', ')
        .replace(/\p{Extended_Pictographic}️?/gu, '')
        .replace(/️/g, '')
        .replace(/[ \n]+$/, '');
}
