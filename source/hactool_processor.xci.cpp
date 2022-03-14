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
#include "hactool_fs_utils.hpp"

namespace ams::hactool {

    namespace {

        constexpr size_t CardInitialDataRegionSize = 0x1000;

        struct XciBodyHeader {
            gc::impl::CardHeaderWithSignature card_header;
            gc::impl::CardHeaderWithSignature card_header_for_sign2;
            gc::impl::Ca10Certificate ca10_cert;
        };

        Result DetermineXciSubStorages(std::shared_ptr<fs::IStorage> *out_key_area, std::shared_ptr<fs::IStorage> *out_body, std::shared_ptr<fs::IStorage> &storage) {
            /* Get the storage size. */
            s64 storage_size;
            R_TRY(storage->GetSize(std::addressof(storage_size)));

            /* Try to read the header from after the initial data region. */
            if (storage_size >= static_cast<s64>(CardInitialDataRegionSize)) {
                gc::impl::CardHeaderWithSignature card_header;
                R_TRY(storage->Read(CardInitialDataRegionSize, std::addressof(card_header), sizeof(card_header)));

                if (card_header.data.magic == gc::impl::CardHeader::Magic) {
                    *out_key_area = std::make_shared<fs::SubStorage>(std::shared_ptr<fs::IStorage>(storage), 0, CardInitialDataRegionSize);
                    *out_body     = std::make_shared<fs::SubStorage>(std::shared_ptr<fs::IStorage>(storage), CardInitialDataRegionSize, storage_size - CardInitialDataRegionSize);
                    R_SUCCEED();
                }
            }

            /* Default to treating the xci as though it has no key area. */
            *out_key_area = nullptr;
            *out_body     = std::make_shared<fs::SubStorage>(storage, 0, storage_size);
            R_SUCCEED();
        }

        Result CreateRootPartitionFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<fs::IStorage> &storage, const gc::impl::CardHeaderWithSignature &header) {
            /* Create meta data. */
            auto meta = std::make_unique<fssystem::Sha256PartitionFileSystemMeta>();
            AMS_ABORT_UNLESS(meta != nullptr);

            /* Initialize meta data. */
            {
                util::optional<u8> salt = util::nullopt;
                if (static_cast<fs::GameCardCompatibilityType>(header.data.encrypted_data.compatibility_type) != fs::GameCardCompatibilityType::Normal) {
                    salt.emplace(header.data.encrypted_data.compatibility_type);
                }
                R_TRY(meta->Initialize(storage.get(), sf::GetNewDeleteMemoryResource(), header.data.partition_fs_header_hash, sizeof(header.data.partition_fs_header_hash), salt));
            }

            /* Create fs. */
            auto fs = std::make_shared<fssystem::Sha256PartitionFileSystem>();
            R_TRY(fs->Initialize(std::move(meta), storage));

            /* Set output. */
            *out = std::move(fs);
            R_SUCCEED();
        }

        Result CreatePartitionFileSystem(std::shared_ptr<fs::fsa::IFileSystem> *out, std::shared_ptr<fs::IStorage> &storage) {
            /* Create meta data. */
            auto meta = std::make_unique<fssystem::Sha256PartitionFileSystemMeta>();
            AMS_ABORT_UNLESS(meta != nullptr);

            s64 size;
            R_ABORT_UNLESS(storage->GetSize(std::addressof(size)));

            /* Initialize meta data. */
            R_TRY(meta->Initialize(storage.get(), sf::GetNewDeleteMemoryResource()));

            /* Create fs. */
            auto fs = std::make_shared<fssystem::Sha256PartitionFileSystem>();
            R_TRY(fs->Initialize(std::move(meta), storage));

            /* Set output. */
            *out = std::move(fs);
            R_SUCCEED();
        }

    }

    Result Processor::ProcessAsXci(std::shared_ptr<fs::IStorage> storage, ProcessAsXciContext *ctx) {
        /* Ensure we have a context. */
        ProcessAsXciContext local_ctx{};
        if (ctx == nullptr) {
            ctx = std::addressof(local_ctx);
        }

        /* Set the storage. */
        ctx->storage = std::move(storage);

        /* Decide on storages. */
        R_TRY(DetermineXciSubStorages(std::addressof(ctx->key_area_storage), std::addressof(ctx->body_storage), ctx->storage));

        /* If we have a key area, read the initial data. */
        if (ctx->key_area_storage != nullptr) {
            R_ABORT_UNLESS(ctx->key_area_storage->Read(0, std::addressof(ctx->card_data.initial_data), sizeof(ctx->card_data.initial_data)));
        }

        /* Read the header. */
        XciBodyHeader body_header;
        R_ABORT_UNLESS(ctx->body_storage->Read(0, std::addressof(body_header), sizeof(body_header)));

        /* Make the card header. */
        ctx->card_data.header = body_header.card_header;

        /* Decrypt the card header. */
        ctx->card_data.decrypted_header = ctx->card_data.header;
        R_ABORT_UNLESS(gc::impl::GcCrypto::DecryptCardHeader(std::addressof(ctx->card_data.decrypted_header.data), sizeof(ctx->card_data.decrypted_header.data)));

        /* Set up the headers for ca10 sign2. */
        if (ctx->card_data.header.data.flags & fs::GameCardAttribute_HasHeaderSign2Flag) {
            ctx->card_data.ca10_certificate          = body_header.ca10_cert;
            ctx->card_data.header_for_hash           = body_header.card_header_for_sign2;
            ctx->card_data.decrypted_header_for_hash = ctx->card_data.header_for_hash;
            R_ABORT_UNLESS(gc::impl::GcCrypto::DecryptCardHeader(std::addressof(ctx->card_data.decrypted_header_for_hash.data), sizeof(ctx->card_data.decrypted_header_for_hash.data)));
        } else {
            ctx->card_data.ca10_certificate          = {};
            ctx->card_data.header_for_hash           = ctx->card_data.header;
            ctx->card_data.decrypted_header_for_hash = ctx->card_data.decrypted_header;
        }

        /* Read the T1 cert. */
        R_ABORT_UNLESS(ctx->body_storage->Read(0x7000, std::addressof(ctx->card_data.t1_certificate), sizeof(ctx->card_data.t1_certificate)));

        /* Parse the root partition. */
        {
            /* Create the root partition storage. */
            using AlignmentMatchingStorageForGameCard = fssystem::AlignmentMatchingStorageInBulkRead<1>;
            auto aligned_storage = std::make_shared<AlignmentMatchingStorageForGameCard>(ctx->body_storage, 0x200);

            /* Get the size of the body. */
            s64 body_size;
            R_ABORT_UNLESS(aligned_storage->GetSize(std::addressof(body_size)));

            /* Create sub storage for the root partition. */
            ctx->root_partition.storage = std::make_shared<fs::SubStorage>(std::move(aligned_storage), ctx->card_data.header.data.partition_fs_header_address, body_size - ctx->card_data.header.data.partition_fs_header_address);

            /* Create filesystem for the root partition. */
            if (const auto res = CreateRootPartitionFileSystem(std::addressof(ctx->root_partition.fs), ctx->root_partition.storage, ctx->card_data.decrypted_header); R_FAILED(res)) {
                fprintf(stderr, "[Warning]: Failed to mount the game card root partition: 2%03d-%04d\n", res.GetModule(), res.GetDescription());
            }
        }

        /* Parse all other partitions. */
        if (ctx->root_partition.fs != nullptr) {
            const auto iter_result = fssystem::IterateDirectoryRecursively(ctx->root_partition.fs.get(),
                fs::MakeConstantPath("/"),
                [&] (const fs::Path &, const fs::DirectoryEntry &) -> Result {
                    R_SUCCEED();
                },
                [&] (const fs::Path &, const fs::DirectoryEntry &) -> Result {
                    R_SUCCEED();
                },
                [&] (const fs::Path &path, const fs::DirectoryEntry &) -> Result {
                    ProcessAsXciContext::PartitionData *target_partition = nullptr;

                    if (std::strcmp(path.GetString(), "/update") == 0) {
                        target_partition = std::addressof(ctx->update_partition);
                    } else if (std::strcmp(path.GetString(), "/logo") == 0) {
                        target_partition = std::addressof(ctx->logo_partition);
                    } else if (std::strcmp(path.GetString(), "/normal") == 0) {
                        target_partition = std::addressof(ctx->normal_partition);
                    } else if (std::strcmp(path.GetString(), "/secure") == 0) {
                        target_partition = std::addressof(ctx->secure_partition);
                    } else {
                        fprintf(stderr, "[Warning]: Found unrecognized game card partition (%s)\n", path.GetString());
                    }

                    if (target_partition != nullptr) {
                        if (const auto res = OpenFileStorage(std::addressof(target_partition->storage), ctx->root_partition.fs, path.GetString()); R_SUCCEEDED(res)) {
                            if (const auto res = CreatePartitionFileSystem(std::addressof(target_partition->fs), target_partition->storage); R_FAILED(res)) {
                                fprintf(stderr, "[Warning]: Failed to mount game card partition (%s): 2%03d-%04d\n", path.GetString(), res.GetModule(), res.GetDescription());
                            }
                        } else {
                            fprintf(stderr, "[Warning]: Failed to open game card partition (%s): 2%03d-%04d\n", path.GetString(), res.GetModule(), res.GetDescription());
                        }
                    }

                    R_SUCCEED();
                }
            );
            if (R_FAILED(iter_result)) {
                fprintf(stderr, "[Warning]: Iterating the root partition failed: 2%03d-%04d\n", iter_result.GetModule(), iter_result.GetDescription());
            }
        }

        /* TODO: Recursive processing? */

        /* Print. */
        if (ctx == std::addressof(local_ctx)) {
            this->PrintAsXci(*ctx);
        }

        /* Save. */
        if (ctx == std::addressof(local_ctx)) {
            this->SaveAsXci(*ctx);
        }

        R_SUCCEED();
    }

    void Processor::PrintAsXci(ProcessAsXciContext &ctx) {
        auto _ = this->PrintHeader("XCI");

        /* TODO: Print correct data instead of the secure partition's contents. */
        if (ctx.secure_partition.fs != nullptr) {
            PrintDirectory(ctx.secure_partition.fs, "secure:", "/");
        }

        /* TODO: Non-debug prints. */
        if (ctx.key_area_storage != nullptr) {
            this->PrintBytes("Initial Data", std::addressof(ctx.card_data.initial_data), sizeof(ctx.card_data.initial_data));
        }
        this->PrintBytes("Encrypted Header", std::addressof(ctx.card_data.header), sizeof(ctx.card_data.header));
        this->PrintBytes("Decrypted Header", std::addressof(ctx.card_data.decrypted_header), sizeof(ctx.card_data.decrypted_header));
        this->PrintBytes("Encrypted Header For Hash", std::addressof(ctx.card_data.header_for_hash), sizeof(ctx.card_data.header_for_hash));
        this->PrintBytes("Decrypted Header For Hash", std::addressof(ctx.card_data.decrypted_header_for_hash), sizeof(ctx.card_data.decrypted_header_for_hash));
        this->PrintBytes("T1 Card Cert", std::addressof(ctx.card_data.t1_certificate), sizeof(ctx.card_data.t1_certificate));
        this->PrintBytes("CA10 Cert", std::addressof(ctx.card_data.ca10_certificate), sizeof(ctx.card_data.ca10_certificate));

        AMS_UNUSED(ctx);
    }

    void Processor::SaveAsXci(ProcessAsXciContext &ctx) {
        /* TODO */
        AMS_UNUSED(ctx);
    }

}