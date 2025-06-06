// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections.Generic;
using Xunit;

namespace System.Globalization.Tests
{
    public class DateTimeFormatInfoAbbreviatedDayNames
    {
        [Fact]
        public void AbbreviatedDayNames_GetInvariantInfo_ReturnsExpected()
        {
            Assert.Equal(new string[] { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }, DateTimeFormatInfo.InvariantInfo.AbbreviatedDayNames);
        }

        [Fact]
        public void AbbreviatedDayNames_Get_ReturnsClone()
        {
            var format = new DateTimeFormatInfo();
            Assert.Equal(format.AbbreviatedDayNames, format.AbbreviatedDayNames);
            Assert.NotSame(format.AbbreviatedDayNames, format.AbbreviatedDayNames);
        }

        public static IEnumerable<object[]> AbbreviatedDayNames_Set_TestData()
        {
            yield return new object[] { new string[] { "1", "2", "3", "4", "5", "6", "7" } };
            yield return new object[] { new string[] { "", "", "", "", "", "", "" } };
        }

        public static IEnumerable<object[]> AbbreviatedDayNames_Get_TestData_ICU()
        {
            yield return new object[] { CultureInfo.GetCultureInfo("en-US").DateTimeFormat, new string[] { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" } };
            yield return new object[] { CultureInfo.GetCultureInfo("fr-FR").DateTimeFormat, new string[] { "dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam." } };
        }


        [ConditionalTheory(typeof(PlatformDetection), nameof(PlatformDetection.IsIcuGlobalization))]
        [MemberData(nameof(AbbreviatedDayNames_Get_TestData_ICU))]
        public void AbbreviatedDayNames_Get_ReturnsExpected_ICU(DateTimeFormatInfo format, string[] expected)
        {
            Assert.Equal(expected, format.AbbreviatedDayNames);
        }


        [Theory]
        [MemberData(nameof(AbbreviatedDayNames_Set_TestData))]
        public void AbbreviatedDayNames_Set_GetReturnsExpected(string[] value)
        {
            var format = new DateTimeFormatInfo();
            format.AbbreviatedDayNames = value;
            Assert.Equal(value, format.AbbreviatedDayNames);

            // Does not clone in setter, only in getter.
            value[0] = null;
            Assert.NotSame(value, format.AbbreviatedDayNames);
            Assert.Equal(value, format.AbbreviatedDayNames);
        }

        [Fact]
        public void AbbreviatedDayNames_SetNulValue_ThrowsArgumentNullException()
        {
            var format = new DateTimeFormatInfo();
            AssertExtensions.Throws<ArgumentNullException>("value", () => format.AbbreviatedDayNames = null);
        }

        [Fact]
        public void AbbreviatedDayNames_SetNulValueInValue_ThrowsArgumentNullException()
        {
            var format = new DateTimeFormatInfo();
            AssertExtensions.Throws<ArgumentNullException>("value", () => format.AbbreviatedDayNames = new string[] { "1", "2", "3", null, "5", "6", "7" });
        }

        public static IEnumerable<object[]> AbbreviatedDayNames_SetInvalidLength_TestData()
        {
            yield return new object[] { new string[] { "Sun" } };
            yield return new object[] { new string[] { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Additional" } };
        }

        [Theory]
        [MemberData(nameof(AbbreviatedDayNames_SetInvalidLength_TestData))]
        public void AbbreviatedDayNames_SetInvalidLength_ThrowsArgumentException(string[] value)
        {
            var format = new DateTimeFormatInfo();
            AssertExtensions.Throws<ArgumentException>("value", (() => format.AbbreviatedDayNames = value));
        }

        [Fact]
        public void AbbreviatedDayNames_SetReadOnly_ThrowsInvalidOperationException()
        {
            Assert.Throws<InvalidOperationException>(() => DateTimeFormatInfo.InvariantInfo.AbbreviatedDayNames = new string[] { "1", "2", "3", "4", "5", "6", "7" });
        }
    }
}
