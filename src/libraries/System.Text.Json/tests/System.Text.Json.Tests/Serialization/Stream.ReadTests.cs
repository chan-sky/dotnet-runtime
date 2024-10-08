// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Xunit;

namespace System.Text.Json.Serialization.Tests
{
    public partial class StreamTests
    {
        [Fact]
        public async Task ReadNullArgumentFail()
        {
            await Assert.ThrowsAsync<ArgumentNullException>(async () => await Serializer.DeserializeWrapper<string>((Stream)null));
            await Assert.ThrowsAsync<ArgumentNullException>(async () => await Serializer.DeserializeWrapper((Stream)null, (Type)null));
            await Assert.ThrowsAsync<ArgumentNullException>(async () => await Serializer.DeserializeWrapper((Stream)null, typeof(string)));
            await Assert.ThrowsAsync<ArgumentNullException>(async () => await Serializer.DeserializeWrapper(new MemoryStream(), (Type)null));
        }

        [Fact]
        public async Task ReadSimpleObjectAsync()
        {
            using (MemoryStream stream = new MemoryStream(SimpleTestClass.s_data))
            {
                JsonSerializerOptions options = new JsonSerializerOptions
                {
                    DefaultBufferSize = 1
                };

                SimpleTestClass obj = await Serializer.DeserializeWrapper<SimpleTestClass>(stream, options);
                obj.Verify();
            }
        }

        [Fact]
        public async Task ReadSimpleObjectWithTrailingTriviaAsync()
        {
            byte[] data = Encoding.UTF8.GetBytes(SimpleTestClass.s_json + " /* Multi\r\nLine Comment */\t");
            using (MemoryStream stream = new MemoryStream(data))
            {
                JsonSerializerOptions options = new JsonSerializerOptions
                {
                    DefaultBufferSize = 1,
                    ReadCommentHandling = JsonCommentHandling.Skip,
                };

                SimpleTestClass obj = await Serializer.DeserializeWrapper<SimpleTestClass>(stream, options);
                obj.Verify();
            }
        }

        [Fact]
        public async Task ReadPrimitivesAsync()
        {
            using (MemoryStream stream = new MemoryStream(Encoding.UTF8.GetBytes(@"1")))
            {
                JsonSerializerOptions options = new JsonSerializerOptions
                {
                    DefaultBufferSize = 1
                };

                int i = await Serializer.DeserializeWrapper<int>(stream, options);
                Assert.Equal(1, i);
            }
        }

        [Fact]
        public async Task ReadPrimitivesWithTrailingTriviaAsync()
        {
            using (MemoryStream stream = new MemoryStream(" 1\t// Comment\r\n/* Multi\r\nLine */"u8.ToArray()))
            {
                JsonSerializerOptions options = new JsonSerializerOptions
                {
                    DefaultBufferSize = 1,
                    ReadCommentHandling = JsonCommentHandling.Skip,
                };

                int i = await Serializer.DeserializeWrapper<int>(stream, options);
                Assert.Equal(1, i);
            }
        }

        [Fact]
        public async Task ReadReferenceTypeCollectionPassingNullValueAsync()
        {
            using (MemoryStream stream = new MemoryStream("null"u8.ToArray()))
            {
                IList<object> referenceTypeCollection = await Serializer.DeserializeWrapper<IList<object>>(stream);
                Assert.Null(referenceTypeCollection);
            }
        }

        [Fact]
        public async Task ReadValueTypeCollectionPassingNullValueAsync()
        {
            using (MemoryStream stream = new MemoryStream("null"u8.ToArray()))
            {
                IList<int> valueTypeCollection = await Serializer.DeserializeWrapper<IList<int>>(stream);
                Assert.Null(valueTypeCollection);
            }
        }

        public static IEnumerable<object[]> BOMTestData =>
            new List<object[]>
            {
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, default, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 1 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 2 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 3 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 4 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 15 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49 }, new JsonSerializerOptions { DefaultBufferSize = 15 }, 1111111111111111111 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 16 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49 }, new JsonSerializerOptions { DefaultBufferSize = 16 }, 1111111111111111111 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49 }, new JsonSerializerOptions { DefaultBufferSize = 17 }, 1 },
                new object[] {new byte[] { 0xEF, 0xBB, 0xBF, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49 }, new JsonSerializerOptions { DefaultBufferSize = 17 }, 1111111111111111111 },
            };

        [Theory]
        [MemberData(nameof(BOMTestData))]
        public async Task TestBOMWithSingleJsonValue(byte[] utf8BomAndValueArray, JsonSerializerOptions options, ulong expected)
        {
            ulong value;
            using (Stream stream = new MemoryStream(utf8BomAndValueArray))
            {
                value = await Serializer.DeserializeWrapper<ulong>(stream, options);
            }
            Assert.Equal(expected, value);
        }

        [Fact]
        public async Task TestBOMWithNoJsonValue()
        {
            byte[] utf8BomAndValueArray = new byte[] { 0xEF, 0xBB, 0xBF };
            using (Stream stream = new MemoryStream(utf8BomAndValueArray))
            {
                await Assert.ThrowsAsync<JsonException>(
                    async () => await Serializer.DeserializeWrapper<byte>(stream));
            }
        }

        public static IEnumerable<object[]> BOMWithStreamTestData
        {
            get
            {
                foreach (object[] testData in Yield(100, 6601)) yield return testData;
                foreach (object[] testData in Yield(200, 13201)) yield return testData;
                foreach (object[] testData in Yield(400, 26401)) yield return testData;
                foreach (object[] testData in Yield(800, 52801)) yield return testData;
                foreach (object[] testData in Yield(1600, 105601)) yield return testData;

                IEnumerable<object[]> Yield(int count, int expectedStreamLength)
                {
                    // Use the same stream instance so the tests run faster.
                    Stream stream = CreateStream(count);

                    // Test with both small (1 byte) and default (16K) buffer sizes to encourage
                    // different code paths dealing with buffer re-use and growing.
                    yield return new object[] { stream, count, expectedStreamLength, 1 };
                    yield return new object[] { stream, count, expectedStreamLength, 16 * 1024 };
                }

                static Stream CreateStream(int count)
                {
                    byte[] objBytes = @"{""Test"":{},""Test2"":[],""Test3"":{""Value"":{}},""PersonType"":0,""Id"":2}"u8.ToArray();

                    byte[] utf8Bom = Encoding.UTF8.GetPreamble();

                    var stream = new MemoryStream();

                    stream.Write(utf8Bom, 0, utf8Bom.Length);
                    stream.WriteByte((byte)'[');

                    for (int i = 1; i <= count; i++)
                    {
                        stream.Write(objBytes, 0, objBytes.Length);

                        if (i < count)
                        {
                            stream.WriteByte((byte)',');
                        }
                    }

                    stream.WriteByte((byte)']');
                    return stream;
                }
            }
        }

        [Theory]
        [MemberData(nameof(BOMWithStreamTestData))]
        public async Task TestBOMWithShortAndLongBuffers(Stream stream, int count, int expectedStreamLength, int bufferSize)
        {
            JsonElement[] value;

            JsonSerializerOptions options = new JsonSerializerOptions()
            {
                DefaultBufferSize = bufferSize
            };

            stream.Position = 0;
            value = await Serializer.DeserializeWrapper<JsonElement[]>(stream, options);

            // Verify each element.
            for (int i = 0; i < count; i++)
            {
                VerifyElement(i);
            }

            // Round trip and verify.
            stream.Position = 3; // Skip the BOM.
            string originalString = new StreamReader(stream).ReadToEnd();
            Assert.Equal(expectedStreamLength, originalString.Length);

            string roundTrippedString = JsonSerializer.Serialize(value);
            Assert.Equal(originalString, roundTrippedString);

            void VerifyElement(int index)
            {
                Assert.Equal(JsonValueKind.Object, value[index].GetProperty("Test").ValueKind);
                Assert.Equal(0, value[index].GetProperty("Test").GetPropertyCount());
                Assert.False(value[index].GetProperty("Test").EnumerateObject().MoveNext());
                Assert.Equal(JsonValueKind.Array, value[index].GetProperty("Test2").ValueKind);
                Assert.Equal(0, value[index].GetProperty("Test2").GetArrayLength());
                Assert.Equal(JsonValueKind.Object, value[index].GetProperty("Test3").ValueKind);
                Assert.Equal(JsonValueKind.Object, value[index].GetProperty("Test3").GetProperty("Value").ValueKind);
                Assert.Equal(1, value[index].GetProperty("Test3").GetPropertyCount());
                Assert.Equal(0, value[index].GetProperty("Test3").GetProperty("Value").GetPropertyCount());
                Assert.False(value[index].GetProperty("Test3").GetProperty("Value").EnumerateObject().MoveNext());
                Assert.Equal(0, value[index].GetProperty("PersonType").GetInt32());
                Assert.Equal(2, value[index].GetProperty("Id").GetInt32());
            }
        }

        [Theory]
        [InlineData(1)]
        [InlineData(16)]
        [InlineData(32000)]
        public async Task ReadPrimitiveWithWhitespace(int bufferSize)
        {
            byte[] data = Encoding.UTF8.GetBytes("42" + new string(' ', 16 * 1024));

            JsonSerializerOptions options = new JsonSerializerOptions();
            options.DefaultBufferSize = bufferSize;

            using (MemoryStream stream = new MemoryStream(data))
            {
                int i = await Serializer.DeserializeWrapper<int>(stream, options);
                Assert.Equal(42, i);
                Assert.Equal(16386, stream.Position);
            }
        }

        [Theory]
        [InlineData(1)]
        [InlineData(16)]
        [InlineData(32000)]
        public async Task ReadObjectWithWhitespace(int bufferSize)
        {
            byte[] data = Encoding.UTF8.GetBytes("{}" + new string(' ', 16 * 1024));

            JsonSerializerOptions options = new JsonSerializerOptions();
            options.DefaultBufferSize = bufferSize;

            using (MemoryStream stream = new MemoryStream(data))
            {
                SimpleTestClass obj = await Serializer.DeserializeWrapper<SimpleTestClass>(stream, options);
                Assert.Equal(16386, stream.Position);
            }
        }

        [Theory]
        [InlineData(1)]
        [InlineData(16)]
        [InlineData(32000)]
        public async Task ReadPrimitiveWithWhitespaceAndThenInvalid(int bufferSize)
        {
            byte[] data = Encoding.UTF8.GetBytes("42" + new string(' ', 16 * 1024) + "!");

            JsonSerializerOptions options = new JsonSerializerOptions();
            options.DefaultBufferSize = bufferSize;

            using (MemoryStream stream = new MemoryStream(data))
            {
                JsonException ex = await Assert.ThrowsAsync<JsonException>(async () => await Serializer.DeserializeWrapper<int>(stream, options));
                Assert.Equal(16387, stream.Position);

                // We should get an exception like: '!' is invalid after a single JSON value.
                Assert.Contains("!", ex.Message);
            }
        }

        [Theory]
        [InlineData(1)]
        [InlineData(16)]
        [InlineData(32000)]
        public async Task ReadObjectWithWhitespaceAndThenInvalid(int bufferSize)
        {
            byte[] data = Encoding.UTF8.GetBytes("{}" + new string(' ', 16 * 1024) + "!");

            JsonSerializerOptions options = new JsonSerializerOptions();
            options.DefaultBufferSize = bufferSize;

            using (MemoryStream stream = new MemoryStream(data))
            {
                JsonException ex = await Assert.ThrowsAsync<JsonException>(async () => await Serializer.DeserializeWrapper<SimpleTestClass>(stream, options));
                Assert.Equal(16387, stream.Position);

                // We should get an exception like: '!' is invalid after a single JSON value.
                Assert.Contains("!", ex.Message);
            }
        }

        [Theory]
        [InlineData(typeof(ClassDeserializedFromLargeJson))]
        [InlineData(typeof(StructDeserializedFromLargeJson))]
        [InlineData(typeof(ClassWithSmallConstructorDeserializedFromLargeJson))]
        [InlineData(typeof(ClassWithLargeConstructorDeserializedFromLargeJson))]
        public async Task ReadTypeFromJsonWithLargeIgnoredProperties(Type type)
        {
            const int MinLength = 1024 * 1024; // 1 MiB
            await ReadTypeFromJsonWithLargeIgnoredPropertiesCore(type, MinLength);
        }

        [Theory]
        [OuterLoop]
        [InlineData(typeof(ClassDeserializedFromLargeJson))]
        [InlineData(typeof(StructDeserializedFromLargeJson))]
        [InlineData(typeof(ClassWithSmallConstructorDeserializedFromLargeJson))]
        [InlineData(typeof(ClassWithLargeConstructorDeserializedFromLargeJson))]
        public async Task ReadTypeFromJsonWithLargeIgnoredProperties_OuterLoop(Type type)
        {
            const long MinLength = 5L * 1024 * 1024 * 1024; // 5 GiB

            if (Serializer.ForceSmallBufferInOptions)
            {
                // Using tiny serialization buffers would take too long to process a file that large (~2 minutes).
                return;
            }

            await ReadTypeFromJsonWithLargeIgnoredPropertiesCore(type, MinLength);
        }

        private async Task ReadTypeFromJsonWithLargeIgnoredPropertiesCore(Type type, long minLength)
        {
            using var stream = new ChunkedReaderStream(GenerateLargeJsonObjectAsFragments(minLength));
            var result = (ITypeDeserializedFromLargeJson)await Serializer.DeserializeWrapper(stream, type);
            Assert.Equal(42, result.FirstValue);
            Assert.Equal(42, result.LastValue);

            static IEnumerable<byte[]> GenerateLargeJsonObjectAsFragments(long minLength)
            {
                long length = 0;

                byte[] documentStart = """{ "FirstValue" : 42, "LargeIgnoredProperty" : { """u8.ToArray();
                yield return documentStart;
                length += documentStart.Length;

                byte[] nestedPropertyValue = "\"Property\" : \"This is a rather large property value that should help contribute to the bulk of the ignored payload.\""u8.ToArray();
                byte[] commaSeparator = ", "u8.ToArray();
                while (true)
                {
                    yield return nestedPropertyValue;
                    length += nestedPropertyValue.Length;

                    if (length < minLength)
                    {
                        yield return commaSeparator;
                    }
                    else
                    {
                        break;
                    }
                }

                yield return """ }, "LastValue" : 42 }"""u8.ToArray();
            }
        }

        public interface ITypeDeserializedFromLargeJson
        {
            int FirstValue { get; }
            int LastValue { get; } 
        }

        public class ClassDeserializedFromLargeJson : ITypeDeserializedFromLargeJson
        {
            public int FirstValue { get; set; }
            public int LastValue { get; set; }
        }

        public struct StructDeserializedFromLargeJson : ITypeDeserializedFromLargeJson
        {
            public int FirstValue { get; set; }
            public int LastValue { get; set; }
        }

        public class ClassWithSmallConstructorDeserializedFromLargeJson(int firstValue, int lastValue) : ITypeDeserializedFromLargeJson
        {
            public int FirstValue { get; } = firstValue;
            public int LastValue { get; } = lastValue;
        }

        public class ClassWithLargeConstructorDeserializedFromLargeJson(
                int firstValue, int lastValue, int unusedParam2, int unusedParam3, int unusedParam4,
                int unusedParam5, int unusedParam6, int unusedParam7, int unusedParam8, int unusedParam9) : ITypeDeserializedFromLargeJson
        {
            public int FirstValue { get; } = firstValue;
            public int LastValue { get; } = lastValue;
            public int UnusedParam2 { get; } = unusedParam2;
            public int UnusedParam3 { get; } = unusedParam3;
            public int UnusedParam4 { get; } = unusedParam4;
            public int UnusedParam5 { get; } = unusedParam5;
            public int UnusedParam6 { get; } = unusedParam6;
            public int UnusedParam7 { get; } = unusedParam7;
            public int UnusedParam8 { get; } = unusedParam8;
            public int UnusedParam9 { get; } = unusedParam9;
        }
    }
}
