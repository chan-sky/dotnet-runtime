// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Security.Cryptography
{
    internal static partial class CngPkcs8
    {
        internal struct Pkcs8Response
        {
            internal CngKey Key;

            internal string? GetAlgorithmGroup()
            {
                return Key.AlgorithmGroup!.AlgorithmGroup;
            }

            internal void FreeKey()
            {
                Key.Dispose();
            }
        }

        private static Pkcs8Response ImportPkcs8(ReadOnlySpan<byte> keyBlob)
        {
            CngKey key = CngKey.Import(keyBlob, CngKeyBlobFormat.Pkcs8PrivateBlob);
            key.ExportPolicy = CngExportPolicies.AllowExport | CngExportPolicies.AllowPlaintextExport;

            return new Pkcs8Response
            {
                Key = key,
            };
        }

        private static Pkcs8Response ImportPkcs8(
            ReadOnlySpan<byte> keyBlob,
            ReadOnlySpan<char> password)
        {
            CngKey key = CngKey.ImportEncryptedPkcs8(keyBlob, password);
            key.ExportPolicy = CngExportPolicies.AllowExport | CngExportPolicies.AllowPlaintextExport;

            return new Pkcs8Response
            {
                Key = key,
            };
        }

        internal static bool AllowsOnlyEncryptedExport(CngKey key)
        {
            const CngExportPolicies Exportable = CngExportPolicies.AllowPlaintextExport | CngExportPolicies.AllowExport;
            return (key.ExportPolicy & Exportable) == CngExportPolicies.AllowExport;
        }
    }
}
