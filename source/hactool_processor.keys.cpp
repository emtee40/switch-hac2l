/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stratosphere.hpp>
#include <exosphere/pkg1.hpp>
#include "hactool_processor.hpp"

namespace ams::hactool {

    namespace {

        constexpr size_t AesKeySize = crypto::AesEncryptor128::KeySize;
        constexpr size_t RsaKeySize = crypto::Rsa2048PssSha256Verifier::ModulusSize;

        struct KeySet {
            u8 secure_boot_key[AesKeySize];                                      /* Secure boot key for use in key derivation. NOTE: CONSOLE UNIQUE. */
            u8 tsec_key[AesKeySize];                                             /* TSEC key for use in key derivation. NOTE: CONSOLE UNIQUE. */
            u8 device_key[AesKeySize];                                           /* Device key used to derive some FS keys. NOTE: CONSOLE UNIQUE. */
            u8 keyblob_keys[pkg1::KeyGeneration_Max][AesKeySize];                /* Actual keys used to decrypt keyblobs. NOTE: CONSOLE UNIQUE.*/
            u8 keyblob_mac_keys[pkg1::KeyGeneration_Max][AesKeySize];            /* Keys used to validate keyblobs. NOTE: CONSOLE UNIQUE. */
            u8 encrypted_keyblobs[pkg1::KeyGeneration_Max][0xB0];                /* Actual encrypted keyblobs (EKS). NOTE: CONSOLE UNIQUE. */
            u8 mariko_aes_class_keys[0xC][AesKeySize];                           /* AES Class Keys set by mariko bootrom. */
            u8 mariko_kek[AesKeySize];                                           /* Key Encryption Key for mariko. */
            u8 mariko_bek[AesKeySize];                                           /* Boot Encryption Key for mariko. */
            u8 keyblobs[pkg1::KeyGeneration_Max][0x90];                          /* Actual decrypted keyblobs (EKS). */
            u8 keyblob_key_sources[pkg1::KeyGeneration_Max][AesKeySize];         /* Seeds for keyblob keys. */
            u8 keyblob_mac_key_source[AesKeySize];                               /* Seed for keyblob MAC key derivation. */
            u8 tsec_root_kek[AesKeySize];                                        /* Used to generate TSEC root keys. */
            u8 package1_mac_kek[AesKeySize];                                     /* Used to generate Package1 MAC keys. */
            u8 package1_kek[AesKeySize];                                         /* Used to generate Package1 keys. */
            u8 tsec_auth_signatures[pkg1::KeyGeneration_Max][AesKeySize];        /* Auth signatures, seeds for tsec root key/package1 mac kek/package1 key on 6.2.0+. */
            u8 tsec_root_keys[pkg1::KeyGeneration_Max][AesKeySize];              /* Key for master kek decryption, from TSEC firmware on 6.2.0+. */
            u8 master_kek_sources[pkg1::KeyGeneration_Max][AesKeySize];          /* Seeds for firmware master keks. */
            u8 mariko_master_kek_sources[pkg1::KeyGeneration_Max][AesKeySize];   /* Seeds for firmware master keks (Mariko). */
            u8 master_keks[pkg1::KeyGeneration_Max][AesKeySize];                 /* Firmware master keks, stored in keyblob prior to 6.2.0. */
            u8 master_key_source[AesKeySize];                                    /* Seed for master key derivation. */
            u8 master_keys[pkg1::KeyGeneration_Max][AesKeySize];                 /* Firmware master keys. */
            u8 package1_mac_keys[pkg1::KeyGeneration_Max][AesKeySize];           /* Package1 MAC keys. */
            u8 package1_keys[pkg1::KeyGeneration_Max][AesKeySize];               /* Package1 keys. */
            u8 package2_keys[pkg1::KeyGeneration_Max][AesKeySize];               /* Package2 keys. */
            u8 package2_key_source[AesKeySize];                                  /* Seed for Package2 key. */
            u8 per_console_key_source[AesKeySize];                               /* Seed for Device key. */
            u8 aes_kek_generation_source[AesKeySize];                            /* Seed for GenerateAesKek, usecase + generation 0. */
            u8 aes_key_generation_source[AesKeySize];                            /* Seed for GenerateAesKey. */
            u8 key_area_key_application_source[AesKeySize];                      /* Seed for kaek 0. */
            u8 key_area_key_ocean_source[AesKeySize];                            /* Seed for kaek 1. */
            u8 key_area_key_system_source[AesKeySize];                           /* Seed for kaek 2. */
            u8 titlekek_source[AesKeySize];                                      /* Seed for titlekeks. */
            u8 header_kek_source[AesKeySize];                                    /* Seed for header kek. */
            u8 sd_card_kek_source[AesKeySize];                                   /* Seed for SD card kek. */
            u8 sd_card_nca_key_source[0x20];                                     /* Seed for SD card encryption keys. */
            u8 sd_card_save_key_source[0x20];                                    /* Seed for SD card encryption keys. */
            u8 save_mac_kek_source[AesKeySize];                                  /* Seed for save kek. */
            u8 save_mac_key_source[AesKeySize];                                  /* Seed for save key. */
            u8 header_key_source[pkg1::KeyGeneration_Max];                       /* Seed for NCA header key. */
            u8 header_key[pkg1::KeyGeneration_Max];                              /* NCA header key. */
            u8 titlekeks[pkg1::KeyGeneration_Max][AesKeySize];                   /* Title key encryption keys. */
            u8 key_area_keys[pkg1::KeyGeneration_Max][3][AesKeySize];            /* Key area encryption keys. */
            u8 xci_header_key[AesKeySize];                                       /* Key for XCI partially encrypted header. */
            u8 save_mac_key[AesKeySize];                                         /* Key used to sign savedata. */
            u8 sd_card_keys[2][pkg1::KeyGeneration_Max];
            u8 nca_hdr_fixed_key_moduli[2][RsaKeySize];                          /* NCA header fixed key RSA pubk. */
            u8 acid_fixed_key_moduli[2][RsaKeySize];                             /* ACID fixed key RSA pubk. */
            u8 package2_fixed_key_modulus[RsaKeySize];                           /* Package2 Header RSA pubk. */
        };

        constinit KeySet g_keyset{};

        bool IsZero(const void *data, size_t size) {
            const u8 *data8 = static_cast<const u8 *>(data);

            for (size_t i = 0; i < size; ++i) {
                if (data8[i] != 0) {
                    return false;
                }
            }

            return true;
        }

        class AesEncryptor128 : public crypto::AesEncryptor128 {
            public:
                AesEncryptor128(const void *key) {
                    crypto::AesEncryptor128::Initialize(key, KeySize);
                }

                void EncryptBlock(void *dst, const void *src) {
                    crypto::AesEncryptor128::EncryptBlock(dst, BlockSize, src, BlockSize);
                }
        };

        class AesDecryptor128 : public crypto::AesDecryptor128 {
            public:
                AesDecryptor128(const void *key) {
                    crypto::AesDecryptor128::Initialize(key, KeySize);
                }

                void DecryptBlock(void *dst, const void *src) {
                    crypto::AesDecryptor128::DecryptBlock(dst, BlockSize, src, BlockSize);
                }
        };

        void InitializeKeySet(KeySet &ks, bool dev) {
            AMS_UNUSED(ks, dev);
        }

        void DeriveKeys(KeySet &ks) {
            /* Helper macros. */
            #define SKIP_IF_UNSET(v) ({ if (IsZero(v, sizeof(v))) { continue; } })

            /* Derive keyblob keys. */
            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_6_2_0; ++gen) {
                SKIP_IF_UNSET(ks.secure_boot_key);
                SKIP_IF_UNSET(ks.tsec_key);
                SKIP_IF_UNSET(ks.keyblob_key_sources[gen]);

                AesDecryptor128(ks.tsec_key).DecryptBlock(ks.keyblob_keys[gen], ks.keyblob_key_sources[gen]);
                AesDecryptor128(ks.secure_boot_key).DecryptBlock(ks.keyblob_keys[gen], ks.keyblob_keys[gen]);

                SKIP_IF_UNSET(ks.keyblob_mac_key_source);

                AesDecryptor128(ks.keyblob_keys[gen]).DecryptBlock(ks.keyblob_mac_keys[gen], ks.keyblob_mac_key_source);

                if (gen == pkg1::KeyGeneration_1_0_0 && !IsZero(ks.per_console_key_source, sizeof(ks.per_console_key_source))) {
                    AesDecryptor128(ks.keyblob_keys[gen]).DecryptBlock(ks.device_key, ks.per_console_key_source);
                }
            }

            /* Decrypt keyblobs. */
            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_6_2_0; ++gen) {
                SKIP_IF_UNSET(ks.keyblob_keys);
                SKIP_IF_UNSET(ks.keyblob_mac_keys);
                SKIP_IF_UNSET(ks.encrypted_keyblobs);

                /* TODO: Cmac */

                /* TODO: Keyblobs */
            }

            /* Set package1 key/master kek via keyblobs. */
            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_6_2_0; ++gen) {
                if (!IsZero(ks.keyblobs[gen] + 0x80, AesKeySize)) {
                    std::memcpy(ks.package1_keys[gen], ks.keyblobs[gen] + 0x80, AesKeySize);
                }

                if (!IsZero(ks.keyblobs[gen] + 0x00, AesKeySize)) {
                    std::memcpy(ks.master_keks[gen], ks.keyblobs[gen] + 0x00, AesKeySize);
                }
            }

            /* Derive newer keydata via tsec. */
            for (int gen = pkg1::KeyGeneration_6_2_0; gen < pkg1::KeyGeneration_Max; ++gen) {
                SKIP_IF_UNSET(ks.tsec_auth_signatures[gen - pkg1::KeyGeneration_6_2_0]);

                /* TODO: Derive tsec root keys, newer pk11 keys. */
            }

            /* TODO: Derive master keks via tsec root keys. */

            /* Derive master keks with mariko keydata, preferring thse to other sources. */
            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_Max; ++gen) {
                SKIP_IF_UNSET(ks.mariko_kek);
                SKIP_IF_UNSET(ks.mariko_master_kek_sources[gen]);

                AesDecryptor128(ks.mariko_kek).DecryptBlock(ks.master_keks[gen], ks.mariko_master_kek_sources[gen]);
            }

            /* Derive master keys. */
            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_Max; ++gen) {
                SKIP_IF_UNSET(ks.master_key_source);
                SKIP_IF_UNSET(ks.master_keks[gen]);

                AesDecryptor128(ks.master_keks[gen]).DecryptBlock(ks.master_keys[gen], ks.master_key_source);
            }

            /* TODO: Further keygen. */
        }

        void DecodeHex(u8 *dst, const char *src, size_t size) {
            const auto len = std::strlen(src);
            if (static_cast<size_t>(len) != 2 * size) {
                fprintf(stderr, "[Warning]: Encounter malformed value (length %" PRIuZ " != expected %" PRIuZ ")\n", static_cast<size_t>(len), 2 * size);
            }

            auto HexToI = [] (char c) ALWAYS_INLINE_LAMBDA {
                if ('a' <= c && c <= 'f') {
                    return 0xA + (c - 'a');
                } else if ('A' <= c && c <= 'F') {
                    return 0xA + (c - 'A');
                } else if ('0' <= c && c <= '9') {
                    return 0x0 + (c - '0');
                } else {
                    return 0;
                }
            };

            for (size_t i = 0; i < size; ++i) {
                dst[i] = (HexToI(src[2 * i + 0]) << 4) | (HexToI(src[2 * i + 1]) << 0);
            }
        }

        void LoadTitleKey(fssrv::impl::ExternalKeyManager &km, const char *key, const char *value) {
            const size_t key_len = std::strlen(key);
            if (key_len % 2 != 0) {
                fprintf(stderr, "[Warning]: Rights Id %s has malformed id (odd number of characters)\n", key);
                return;
            }

            if (key_len != 32) {
                fprintf(stderr, "[Warning]: Rights Id %s has malformed id (wrong number of characters)\n", key);
                return;
            }

            for (size_t i = 0; i < key_len; ++i) {
                const char c = key[i];
                if (!('0' <= c && c <= '9') && !('a' <= c && c <= 'f') && !('A' <= c && c <= 'F')) {
                    fprintf(stderr, "[Warning]: Rights Id %s has malformed id (not hexadecimal)\n", key);
                    return;
                }
            }

            const size_t value_len = std::strlen(value);
            if (value_len != 32) {
                fprintf(stderr, "[Warning]: Rights Id %s has malformed value (wrong number of characters)\n", value);
            }

            /* Decode the rights id. */
            fs::RightsId rights_id = {};
            DecodeHex(rights_id.data, key, sizeof(rights_id.data));

            /* Decode the key. */
            spl::AccessKey access_key = {};
            DecodeHex(access_key.data, value, sizeof(access_key));

            /* Register with the key manager. */
            km.Register(rights_id, access_key);
        }

        void LoadExternalKey(KeySet &ks, const char *key, const char *value) {
            bool matched_key = false;
            char test_name[0x100];
            #define TEST_KEY(kn) if (std::strcmp(key, #kn) == 0) { matched_key = true; DecodeHex(ks.kn, value, sizeof(ks.kn)); }
            #define TEST_KEY_WITH_GEN(kn, gn) if (std::strcmp(key, ({ util::TSNPrintf(test_name, sizeof(test_name), #kn "_%02" PRIx32 "", static_cast<u32>(gn)); test_name; })) == 0) { matched_key = true; DecodeHex(ks.kn##s[gn], value, sizeof(ks.kn##s[gn])); }

            TEST_KEY(aes_kek_generation_source);
            TEST_KEY(aes_key_generation_source);
            TEST_KEY(key_area_key_application_source);
            TEST_KEY(key_area_key_ocean_source);
            TEST_KEY(key_area_key_system_source);
            TEST_KEY(titlekek_source);
            TEST_KEY(header_kek_source);
            TEST_KEY(header_key_source);
            TEST_KEY(header_key);
            TEST_KEY(package2_key_source);
            TEST_KEY(per_console_key_source);
            TEST_KEY(xci_header_key);
            TEST_KEY(sd_card_kek_source);
            TEST_KEY(sd_card_nca_key_source);
            TEST_KEY(sd_card_save_key_source);
            TEST_KEY(save_mac_kek_source);
            TEST_KEY(save_mac_key_source);
            TEST_KEY(master_key_source);
            TEST_KEY(keyblob_mac_key_source);
            TEST_KEY(secure_boot_key);
            TEST_KEY(tsec_key);
            TEST_KEY(mariko_kek);
            TEST_KEY(mariko_bek);
            TEST_KEY(tsec_root_kek);
            TEST_KEY(package1_mac_kek);
            TEST_KEY(package1_kek);

            /* TODO: beta_nca0_exponent */

            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_6_2_0; ++gen) {
                TEST_KEY_WITH_GEN(keyblob_key_source, gen);
                TEST_KEY_WITH_GEN(keyblob_key, gen);
                TEST_KEY_WITH_GEN(keyblob_mac_key, gen);
                TEST_KEY_WITH_GEN(encrypted_keyblob, gen);
                TEST_KEY_WITH_GEN(keyblob, gen);

                TEST_KEY_WITH_GEN(mariko_master_kek_source, gen);
            }
            for (int gen = pkg1::KeyGeneration_6_2_0; gen < pkg1::KeyGeneration_Max; ++gen) {
                TEST_KEY_WITH_GEN(tsec_auth_signature, gen - pkg1::KeyGeneration_6_2_0);
                TEST_KEY_WITH_GEN(tsec_root_key, gen - pkg1::KeyGeneration_6_2_0);

                TEST_KEY_WITH_GEN(master_kek_source, gen);
                TEST_KEY_WITH_GEN(mariko_master_kek_source, gen);

                TEST_KEY_WITH_GEN(package1_mac_key, gen);
            }

            for (int idx = 0; idx < 0xC; ++idx) {
                TEST_KEY_WITH_GEN(mariko_aes_class_key, idx);
            }

            for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_Max; ++gen) {
                TEST_KEY_WITH_GEN(master_kek, gen);
                TEST_KEY_WITH_GEN(master_key, gen);

                TEST_KEY_WITH_GEN(package1_key, gen);
                TEST_KEY_WITH_GEN(package2_key, gen);

                TEST_KEY_WITH_GEN(titlekek, gen);

                util::TSNPrintf(test_name, sizeof(test_name), "key_area_key_application_%02" PRIx32 "", static_cast<u32>(gen));
                if (std::strcmp(key, test_name) == 0) {
                    matched_key = 1;
                    DecodeHex(ks.key_area_keys[gen][0], value, sizeof(ks.key_area_keys[gen][0]));
                }

                util::TSNPrintf(test_name, sizeof(test_name), "key_area_key_ocean_%02" PRIx32 "", static_cast<u32>(gen));
                if (std::strcmp(key, test_name) == 0) {
                    matched_key = 1;
                    DecodeHex(ks.key_area_keys[gen][1], value, sizeof(ks.key_area_keys[gen][1]));
                }

                util::TSNPrintf(test_name, sizeof(test_name), "key_area_key_system_%02" PRIx32 "", static_cast<u32>(gen));
                if (std::strcmp(key, test_name) == 0) {
                    matched_key = 1;
                    DecodeHex(ks.key_area_keys[gen][2], value, sizeof(ks.key_area_keys[gen][2]));
                }
            }

            if (!matched_key) {
                fprintf(stderr, "[Warning]: Failed to match key \"%s\", (value \"%s\")\n", key, value);
            }
        }

        void ProcessKeyValue(const char *path, const char *key, const char *value, auto f) {
            AMS_UNUSED(path);

            const size_t value_len = std::strlen(value);
            if (value_len % 2 != 0) {
                fprintf(stderr, "[Warning]: Key %s has malformed value (odd number of characters)\n", key);
                return;
            }

            for (size_t i = 0; i < value_len; ++i) {
                const char c = value[i];
                if (!('0' <= c && c <= '9') && !('a' <= c && c <= 'f') && !('A' <= c && c <= 'F')) {
                    fprintf(stderr, "[Warning]: Key %s has malformed value (not hexadecimal)\n", key);
                    return;
                }
            }

            f(key, value);
        }

        void ProcessKeyValueFile(const char *path, char *buf, s64 file_size, auto f) {
            /* Load keys from the file data. */
            s64 ofs = 0;
            while (ofs < file_size) {
                /* Skip from start of line to start of key. */
                if (buf[ofs] == '\n' || buf[ofs] == '\r' || buf[ofs] == '\x00' || buf[ofs] == ' ' || buf[ofs] == '\t') {
                    ++ofs;
                    continue;
                }

                /* Parse/validate the key. */
                char *key = std::addressof(buf[ofs]);
                s64 kend;
                for (kend = ofs; kend < file_size; ++kend) {
                    if (buf[kend] == ' ' || buf[kend] == ',' || buf[kend] == '\t' || buf[kend] == '=') {
                        break;
                    }

                    if (buf[kend] == '\x00') {
                        fprintf(stderr, "[Warning]: Encountered malformed key (%s) inside key file (%s)\n", key, path);
                        return;
                    }

                    if ('A' <= buf[kend] && buf[kend] <= 'Z') {
                        buf[kend] = 'a' + (buf[kend] - 'A');
                        continue;
                    }

                    if (buf[kend] != '_' && !('0' <= buf[kend] && buf[kend] <= '9') && !('a' <= buf[kend] && buf[kend] <= 'z')) {
                        buf[kend + 1] = '\x00';
                        fprintf(stderr, "[Warning]: Encountered malformed key (%s) inside key file (%s)\n", key, path);
                        return;
                    }
                }

                /* Verify we still have data. */
                if (kend == file_size) {
                    buf[kend] = '\x00';
                    fprintf(stderr, "[Warning]: Encountered truncated key-value pair (key = %s) inside key file (%s)\n", key, path);
                    return;
                }

                /* We should be after a key now, so skip a delimiter. */
                ofs = kend;
                if (buf[ofs] == '=' || buf[ofs] == ',') {
                    buf[ofs++] = '\x00';
                } else {
                    buf[ofs++] = '\x00';
                    while (ofs < file_size && (buf[ofs] == ' ' || buf[ofs] == '\t')) {
                        ++ofs;
                    }
                    if (ofs == file_size || (buf[ofs] != '=' && buf[ofs] != ',')) {
                        fprintf(stderr, "[Warning]: Encountered malformed key-value pair (key = %s) inside key file (%s)\n", key, path);
                        return;
                    }
                    buf[ofs++] = '\x00';
                }

                /* Key must not be empty. */
                if (*key == '\x00') {
                    fprintf(stderr, "[Warning]: Encountered malformed empty key inside key file (%s)\n", path);
                    return;
                }

                /* Skip spaces to get to value. */
                while (ofs < file_size && (buf[ofs] == ' ' || buf[ofs] == '\t')) {
                    ++ofs;
                }

                if (ofs == file_size) {
                    fprintf(stderr, "[Warning]: Encountered missing value (for key = %s) inside key file (%s)\n", key, path);
                    return;
                }

                char *value = std::addressof(buf[ofs]);
                s64 vend;
                for (vend = ofs; vend < file_size; ++vend) {
                    if (buf[vend] == '\n' || buf[vend] == '\r' || buf[vend] == ' ' || buf[vend] == '\t') {
                        buf[vend] = '\x00';
                        break;
                    }
                }
                ofs = vend;

                /* Parse key/value. */
                ProcessKeyValue(path, key, value, f);
            }
        }

        void LoadKeyValueFile(const char *path, auto f) {
            /* Open the file. */
            fs::FileHandle file;
            if (const auto res = fs::OpenFile(std::addressof(file), path, fs::OpenMode_Read); R_FAILED(res)) {
                fprintf(stderr, "[Warning]: failed to open key file (%s): 2%03d-%04d\n", path, res.GetModule(), res.GetDescription());
                return;
            }
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            /* Get the file size. */
            s64 file_size;
            if (const auto res = fs::GetFileSize(std::addressof(file_size), file); R_FAILED(res)) {
                fprintf(stderr, "[Warning]: failed to get key file size (%s): 2%03d-%04d\n", path, res.GetModule(), res.GetDescription());
                return;
            }

            /* Allocate buffer for the file. */
            auto buf = std::make_unique<char[]>(file_size);
            if (buf == nullptr) {
                fprintf(stderr, "[Warning]: failed to allocate memory for key file (%s)\n", path);
                return;
            }

            /* Read the file. */
            if (const auto res = fs::ReadFile(file, 0, buf.get(), file_size); R_FAILED(res)) {
                fprintf(stderr, "[Warning]: failed to read key file (%s): 2%03d-%04d\n", path, res.GetModule(), res.GetDescription());
                return;
            }

            ProcessKeyValueFile(path, buf.get(), file_size, f);
        }

    }

    void Processor::PresetInternalKeys() {
        /* Setup the initial keyset. */
        InitializeKeySet(g_keyset, m_options.dev);

        /* Load external keys. */
        if (m_options.key_file_path != nullptr) {
            LoadKeyValueFile(m_options.key_file_path, [](const char *key, const char *value) {
                LoadExternalKey(g_keyset, key, value);
            });
        }

        /* Derive keys. */
        DeriveKeys(g_keyset);

        /* Set all master keys with spl. */
        for (int gen = pkg1::KeyGeneration_1_0_0; gen < pkg1::KeyGeneration_Max; ++gen) {
            spl::smc::PresetInternalKey(reinterpret_cast<const spl::AesKey *>(std::addressof(g_keyset.master_keys[gen])), gen, false);
        }

        /* Set internal keys for gamecard library. */
        if (const auto res = gc::impl::EmbeddedDataHolder::SetLibraryEmbeddedKeys(m_options.dev); R_FAILED(res)) {
            fprintf(stderr, "[Warning]: Failed to preset internal keys for gamecard library (2%03d-%04d). Is master_key_04 correct?\n", res.GetModule(), res.GetDescription());
        }

        /* Load titlekeys. */
        if (m_options.titlekey_path != nullptr) {
            LoadKeyValueFile(m_options.titlekey_path, [&](const char *key, const char *value) {
                LoadTitleKey(m_external_nca_key_manager, key, value);
            });
        }
    }

}