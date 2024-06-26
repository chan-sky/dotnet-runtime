// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.IO;
using System.Runtime.Serialization.Formatters.Binary;
using Microsoft.DotNet.RemoteExecutor;
using Xunit;

namespace System.Runtime.Serialization.Formatters.Tests
{
    public static class DisableBitTests
    {
        // these tests only make sense on platforms with both "feature switch" and RemoteExecutor support
        public static bool ShouldRunFullFeatureSwitchEnablementChecks => !PlatformDetection.IsNetFramework && RemoteExecutor.IsSupported;

        // determines whether BinaryFormatter will always fail, regardless of config, on this platform
        public static bool IsBinaryFormatterSuppressedOnThisPlatform => !TestConfiguration.IsBinaryFormatterSupported;

        public static bool IsFeatureSwitchIgnored = !TestConfiguration.IsFeatureSwitchRespected;

        private const string MoreInfoUrl = "https://aka.ms/binaryformatter";

        [ConditionalFact(nameof(IsBinaryFormatterSuppressedOnThisPlatform))]
        public static void DisabledAlwaysInBrowser()
        {
            // First, test serialization

            MemoryStream ms = new MemoryStream();
            BinaryFormatter bf = new BinaryFormatter();
            var ex = Assert.Throws<PlatformNotSupportedException>(() => bf.Serialize(ms, "A string to serialize."));
            Assert.Contains(MoreInfoUrl, ex.Message, StringComparison.Ordinal); // error message should link to the more info URL

            // Then test deserialization

            ex = Assert.Throws<PlatformNotSupportedException>(() => bf.Deserialize(ms));
            Assert.Contains(MoreInfoUrl, ex.Message, StringComparison.Ordinal); // error message should link to the more info URL
        }

        [ConditionalFact(nameof(ShouldRunFullFeatureSwitchEnablementChecks))]
        public static void EnabledThroughFeatureSwitch()
        {
            RemoteInvokeOptions options = new RemoteInvokeOptions();
            options.RuntimeConfigurationOptions[TestConfiguration.EnableBinaryFormatterSwitchName] = bool.TrueString;

            RunRemoteTest(options, TestConfiguration.IsBinaryFormatterSupported);
        }

        [ConditionalFact(nameof(ShouldRunFullFeatureSwitchEnablementChecks))]
        public static void DisabledThroughFeatureSwitch()
        {
            RemoteInvokeOptions options = new RemoteInvokeOptions();
            options.RuntimeConfigurationOptions[TestConfiguration.EnableBinaryFormatterSwitchName] = bool.FalseString;

            bool expectSuccess = TestConfiguration.IsBinaryFormatterSupported;

            if (TestConfiguration.IsFeatureSwitchRespected)
            {
                expectSuccess = false;
            }

            RunRemoteTest(options, expectSuccess);
        }

        private static void RunRemoteTest(RemoteInvokeOptions options, bool expectSuccess)
        {
            if (expectSuccess)
            {
                RemoteExecutor.Invoke(
                    () =>
                    {
                        // Test serialization

                        MemoryStream ms = new MemoryStream();
                        new BinaryFormatter().Serialize(ms, "A string to serialize.");

                        // Test round-trippability

                        ms.Position = 0;
                        object roundTripped = new BinaryFormatter().Deserialize(ms);
                        Assert.Equal("A string to serialize.", roundTripped);
                    },
                    options).Dispose();
            }
            else
            {
                RemoteExecutor.Invoke(
                    () =>
                    {
                        // First, test serialization

                        MemoryStream ms = new MemoryStream();
                        BinaryFormatter bf = new BinaryFormatter();
                        var ex = Assert.ThrowsAny<NotSupportedException>(() => bf.Serialize(ms, "A string to serialize."));
                        Assert.Contains(MoreInfoUrl, ex.Message, StringComparison.Ordinal); // error message should link to the more info URL

                        // Then test deserialization

                        ex = Assert.ThrowsAny<NotSupportedException>(() => bf.Deserialize(ms));
                        Assert.Contains(MoreInfoUrl, ex.Message, StringComparison.Ordinal); // error message should link to the more info URL
                    },
                    options).Dispose();
            }
        }
    }
}
