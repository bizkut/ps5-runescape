/* Official Microsoft 14.44.35211, newer than Jagex's pinned 14.40 bundle.
 * Pins authenticate the complete signed installer, never synthesized MSI state. */
#ifndef RUNESCAPE_REDIST_H
#define RUNESCAPE_REDIST_H
struct rs_redist {
    const char *arch, *name, *url, *sha256;
    unsigned size;
    const char *upgrade[2];
    const char *bundle;
};
static const struct rs_redist rs_redists[] = {
    {"x86", "VC_redist.x86.exe",
     "https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/0C09F2611660441084CE0DF425C51C11E147E6447963C3690F97E0B25C55ED64/VC_redist.x86.exe",
     "0c09f2611660441084ce0df425c51c11e147e6447963c3690f97e0b25c55ed64", 13953392,
     {"{65E5BD06-6392-3027-8C26-853107D3CF1A}", "{C78B8E51-0C65-377E-85D1-282F689FE505}"},
     "{0b5169e3-39da-4313-808e-1f9c0407f3bf}"},
    {"x64", "VC_redist.x64.exe",
     "https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe",
     "cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b", 25635768,
     {"{36F68A90-239C-34DF-B58C-64B30153CE35}", "{9B0BAA88-E15F-3A1F-ACC0-B206E9DDF71C}"},
     "{d8bbe9f9-7c5b-42c6-b715-9ee898a2e515}"},
};
/* Drain an already-registered 14.40 bundle before upgrading, rather than
 * overlapping the new Burn chain with its related-bundle cleanup chain. */
static const struct rs_redist rs_legacy_redists[] = {
    {"x86", "VC_redist.legacy.x86.exe",
     "https://download.visualstudio.microsoft.com/download/pr/9c69db26-cda4-472d-bdae-f2b87f4a0177/A32DD41EAAB0C5E1EAA78BE3C0BB73B48593DE8D97A7510B97DE3FD993538600/VC_redist.x86.exe",
     "a32dd41eaab0c5e1eaa78be3c0bb73b48593de8d97a7510b97de3fd993538600", 13867304,
     {"{65E5BD06-6392-3027-8C26-853107D3CF1A}", "{C78B8E51-0C65-377E-85D1-282F689FE505}"},
     "{47109d57-d746-4f8b-9618-ed6a17cc922b}"},
    {"x64", "VC_redist.legacy.x64.exe",
     "https://download.visualstudio.microsoft.com/download/pr/1754ea58-11a6-44ab-a262-696e194ce543/3642E3F95D50CC193E4B5A0B0FFBF7FE2C08801517758B4C8AEB7105A091208A/VC_redist.x64.exe",
     "3642e3f95d50cc193e4b5a0b0ffbf7fe2c08801517758b4c8aeb7105a091208a", 25397512,
     {"{36F68A90-239C-34DF-B58C-64B30153CE35}", "{9B0BAA88-E15F-3A1F-ACC0-B206E9DDF71C}"},
     "{5af95fd8-a22e-458f-acee-c61bd787178e}"},
};
#endif
