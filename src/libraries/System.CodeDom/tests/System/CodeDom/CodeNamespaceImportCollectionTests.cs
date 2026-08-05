// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using System.Collections;
using System.Collections.Generic;
using System.Collections.Tests;
using Xunit;

namespace System.CodeDom.Tests
{
    public class CodeNamespaceImportCollectionTests : IList_NonGeneric_Tests
    {
        protected override IList NonGenericIListFactory() => new CodeNamespaceImportCollection();

        protected override object CreateT(int seed) => new CodeNamespaceImport(seed.ToString());

        protected override bool NullAllowed => false;
        protected override bool Enumerator_Current_UndefinedOperation_Throws => true;
        protected override bool ICollection_NonGeneric_HasNullSyncRoot => true;

        protected override Type ICollection_NonGeneric_CopyTo_NonZeroLowerBound_ThrowType => typeof(ArgumentOutOfRangeException);
        protected override Type ICollection_NonGeneric_CopyTo_ArrayOfIncorrectReferenceType_ThrowType => typeof(InvalidCastException);
        protected override Type ICollection_NonGeneric_CopyTo_ArrayOfIncorrectValueType_ThrowType => typeof(InvalidCastException);

        [Fact]
        public void Add()
        {
            var collection = new CodeNamespaceImportCollection();
            var value = new CodeNamespaceImport();

            collection.Add(value);
            Assert.Equal(1, collection.Count);
            Assert.Same(value, collection[0]);
        }

        [Fact]
        public void Add_SameNamespace_DoesntAdd()
        {
            var collection = new CodeNamespaceImportCollection();
            var value1 = new CodeNamespaceImport("Namespace");
            collection.Add(value1);

            collection.Add(value1);
            Assert.Equal(1, collection.Count);

            var value2 = new CodeNamespaceImport("Namespace");
            collection.Add(value2);
            Assert.Equal(1, collection.Count);

            // IList.Add has long standing behavior to add the value even if duplicate
            Assert.Equal(1, ((IList)collection).Add(value1));
            Assert.Equal(2, collection.Count);

            var anotherValue = new CodeNamespaceImport("AnotherNamespace");
            Assert.Equal(2, ((IList)collection).Add(anotherValue));
            Assert.Equal(3, collection.Count);

            // Check that we don't add duplicate if the first one was added via IList.Add
            collection.Add(anotherValue);
            Assert.Equal(3, collection.Count);
        }

        [Fact]
        public void Add_Null_ThrowsNullReferenceException()
        {
            var collection = new CodeNamespaceImportCollection();
            Assert.Throws<NullReferenceException>(() => collection.Add(null));
            Assert.Throws<NullReferenceException>(() => ((IList)collection).Add(null));
            Assert.Equal(0, collection.Count);
        }

        [Fact]
        public void Add_Uncastable_ThrowsInvalidCastException()
        {
            var collection = new CodeNamespaceImportCollection();
            Assert.Throws<InvalidCastException>(() => ((IList)collection).Add("string"));
            Assert.Equal(0, collection.Count);
        }

        [Fact]
        public void Insert_Null_ThrowsNullReferenceException()
        {
            var collection = new CodeNamespaceImportCollection();
            Assert.Throws<NullReferenceException>(() => ((IList)collection).Insert(0, null));
            Assert.Equal(0, collection.Count);
        }

        [Fact]
        public void Insert_Uncastable_ThrowsInvalidCastException()
        {
            var collection = new CodeNamespaceImportCollection();
            Assert.Throws<InvalidCastException>(() => ((IList)collection).Insert(0, "string"));
            Assert.Equal(0, collection.Count);
        }

        public static IEnumerable<object[]> AddRange_TestData()
        {
            yield return new object[] { new CodeNamespaceImport[0] };
            yield return new object[] { new CodeNamespaceImport[] { new CodeNamespaceImport() } };
        }

        [Theory]
        [MemberData(nameof(AddRange_TestData))]
        public void AddRange_CodeNamespaceArray_Works(CodeNamespaceImport[] value)
        {
            var collection = new CodeNamespaceImportCollection();
            collection.AddRange(value);
            Assert.Equal(value.Length, collection.Count);
            for (int i = 0; i < value.Length; i++)
            {
                Assert.Same(value[i], collection[i]);
            }
        }

        [Fact]
        public void AddRange_Null_ThrowsArgumentNullException()
        {
            var collection = new CodeNamespaceImportCollection();
            AssertExtensions.Throws<ArgumentNullException>("value", () => collection.AddRange(null));
            AssertExtensions.Throws<ArgumentNullException>("value", () => collection.AddRange(null));
        }

        [Fact]
        public void AddRange_NullObjectInValue_ThrowsNullReferenceException()
        {
            var collection = new CodeNamespaceImportCollection();
            Assert.Throws<NullReferenceException>(() => collection.AddRange(new CodeNamespaceImport[] { null }));
        }

        [Theory]
        [InlineData(-1)]
        [InlineData(0)]
        public void Item_InvalidIndex_ThrowsArgumentOutOfRangeException(int index)
        {
            var collection = new CodeNamespaceCollection();
            AssertExtensions.Throws<ArgumentOutOfRangeException>("index", () => collection[index]);
            AssertExtensions.Throws<ArgumentOutOfRangeException>("index", () => collection[index] = new CodeNamespace());
        }

        [Fact]
        public void Item_Set_Null_ThrowsNullReferenceException()
        {
            var collection = new CodeNamespaceImportCollection();
            var value = new CodeNamespaceImport();
            collection.Add(value);

            Assert.Throws<NullReferenceException>(() => collection[0] = null);
            Assert.Throws<NullReferenceException>(() => ((IList)collection)[0] = null);
            Assert.Equal(value, collection[0]);
            Assert.Equal(1, collection.Count);
        }

        [Fact]
        public void Item_Set_Uncastable_ThrowsInvalidCastException()
        {
            var collection = new CodeNamespaceImportCollection();
            var value = new CodeNamespaceImport();
            collection.Add(value);

            Assert.Throws<InvalidCastException>(() => ((IList)collection)[0] = "string");
            Assert.Equal(value, collection[0]);
            Assert.Equal(1, collection.Count);
        }

        [Fact]
        public void ItemSet_Get_ReturnsExpected()
        {
            var value1 = new CodeNamespace();
            var value2 = new CodeNamespace();
            var collection = new CodeNamespaceCollection();
            collection.Add(value1);

            collection[0] = value2;
            Assert.Equal(1, collection.Count);
            Assert.Same(value2, collection[0]);
        }
    }
}
