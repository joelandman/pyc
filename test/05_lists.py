# Test lists
def main():
    nums = [1, 2, 3, 4, 5]
    print(nums)
    
    # Simple list operations
    total = 0
    i = 0
    while i < len(nums):
        total = total + nums[i]
        i = i + 1
    print(total)

if __name__ == "__main__":
    main()
