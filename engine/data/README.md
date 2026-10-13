# Mixed-input lexical data

`mixed_lexicon.inc` is an adapted subset of **wordfreq 3.1.1**, by Robyn Speer
(2022), https://github.com/rspeer/wordfreq,
https://doi.org/10.5281/zenodo.7199437.

The derived data is licensed **CC BY-SA 4.0**:
https://creativecommons.org/licenses/by-sa/4.0/ . Preserve this attribution and
license when redistributing the data or adaptations. The surrounding engine code
retains its own license.

wordfreq combines freely available frequency statistics, including Google Books
Ngrams (http://books.google.com/ngrams), the Leeds Internet Corpus
(http://corpus.leeds.ac.uk/list.html), Wikipedia (https://www.wikipedia.org),
ParaCrawl (https://paracrawl.eu), OPUS OpenSubtitles 2018
(http://opus.nlpl.eu/OpenSubtitles.php) / OpenSubtitles
(https://www.opensubtitles.org), and SUBTLEX word lists created by Marc Brysbaert
et al. (http://crr.ugent.be/programs-data/subtitle-frequencies).
SUBTLEX is freely available data. See wordfreq's documentation for its sources
and the individual SUBTLEX publications.

Transformations: retain ASCII English words (including alphanumeric terms), contractions and hyphenated words
with Zipf frequency >= 3 as primary entries; retain English entries with
1 <= Zipf < 3 separately, marked by a third TSV field `r`; retain
1–4-character Chinese entries with Zipf >= 2.5;
convert Chinese to Taiwan traditional using OpenCC `s2twp`, merging collisions by
maximum frequency; round Zipf to hundredths; sort lexically and embed as TSV
string chunks. This is a general-language unigram/phrase prior, not a trained
conditional bilingual model or a Taiwan-specific corpus. Source usage is a
snapshot through approximately 2021.

Only primary English entries contribute to dictionary membership, prefix
frequencies and character bigram statistics. Rare entries of at least four
characters provide a soft whole-word score using the same Zipf scale; they do
not independently establish English boundaries or suppress completed phonetic
input merely by appearing in the source dictionary. At a natural Chinese
phrase onset, a rare whole word with Zipf >= 2.5 can expose an otherwise missing
literal cut without gaining hard dictionary or prefix protection. Neither tier
contains a handwritten technical word list.
Unlisted alphabetic compounds may also receive a conservative whole-word prior
when an established word or abbreviation (Zipf >= 2.5) is followed by a common
primary word (Zipf >= 4). Rare prefixes must also end in a vowel or `y`, limiting
this backoff to plausible combining forms rather than arbitrary consonant
fragments that overlap completed phonetic input. This scoring does not add dictionary membership or
create new literal boundaries.
The decoder also checks the active layout's natural completed reading prefixes;
an explicit non-neutral tone suppresses this synthetic compound backoff, while
the prior for an actual whole English dictionary entry remains available.

Regenerate with Python 3, `wordfreq==3.1.1` and
`opencc-python-reimplemented==0.1.7`:

```sh
python engine/tools/generate_mixed_lexicon.py
```

Generation is an explicit maintenance step. Normal builds use the checked-in
data, require no Python, download nothing, and embed the data in the engine so
both Linux and macOS use identical resources.
