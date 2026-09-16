import traceback

blech = 1
try:
    bluch
except NameError as e:
    print(e.name)
    t = "".join(traceback.format_exception(e))
    print("suggest" if "blech" in t else "nosuggest")
