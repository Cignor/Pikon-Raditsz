import re

filepath = r'h:\0000_CODE\01_collider_pyo\juce\Source\audio\modules\SdrDemodulator.h'

with open(filepath, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace modulationIndex assignments with comments
content = re.sub(r'modulationIndex = 0\.8f;', '// FM gain calc in demodulateWFM()', content)
content = re.sub(r'modulationIndex = 0\.05f;', '// FM gain calc in demodulateNFM()', content)
content = re.sub(r'modulationIndex = 1\.0f;', '// No FM params for AM', content)

with open(filepath, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(content)

print('Done! Replaced modulationIndex references.')
