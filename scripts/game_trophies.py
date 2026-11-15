# SPDX-License-Identifier: GPL-2.0-or-later
"""Guitar Hero Live's trophies (CUSA02410) for the launcher's Achievements page: their texts and grades in
trophy id order (the runtime's own copy is src/runtime_services.c, game_trophies; a test keeps
the two the same), the icons from the dump's trophy00.trp and the unlocked set the runtime saves.

The texts in trophy00.trp are encrypted (TROP.ESFM); its icons (TROP000.PNG...) are not.
"""
import struct
from pathlib import Path

GRADES = ('Platinum', 'Gold', 'Silver', 'Bronze')
# (name, description, grade) by trophy id.
TROPHIES = (
    ('Platinum Trophy', 'Collect all Guitar Hero Live trophies.', 'Platinum'),  # 0
    ('Triple Down', 'Complete 3 songs in Quickplay.', 'Bronze'),  # 1
    ('Posi', 'Finish every song of a single set with a positive crowd in GH Live.', 'Bronze'),  # 2
    ('Encore...', 'Complete a set in GH Live.', 'Bronze'),  # 3
    ('Nose To The Grind Stone', 'Play GHTV for 10 hours.', 'Bronze'),  # 4
    ('Pointdexter', 'Score 100,000,000 points in GHTV.', 'Gold'),  # 5
    ('OHHHH, Pretty', 'Acquire a Player Card in GHTV.', 'Bronze'),  # 6
    ('Partial Universe', 'Get 1,000 stars in GHTV by playing songs.', 'Bronze'),  # 7
    ('Replay King', 'Score 20,000,000 points in Quickplay.', 'Bronze'),  # 8
    ('Credit Is Due', 'Watch the credits. Twice. To completion.', 'Bronze'),  # 9
    ('The Stranger', 'Play any On Demand song in GHTV with Lefty Flip turned on and get 3 Stars.', 'Bronze'),  # 10
    ('The Right Combination', 'Get a Full Combo in a song in GHTV.', 'Bronze'),  # 11
    ('Not Bad', 'Get 100% in two songs in Quickplay on Regular difficulty.', 'Bronze'),  # 12
    ('Toppest Of Top Drawers', 'Get to Level 20 in GHTV.', 'Bronze'),  # 13
    ('Centurion', 'Use 100 Plays in GHTV.', 'Bronze'),  # 14
    ("Do It You Won't Do It", 'Switch the crowd from negative to positive in the last ten seconds of a song in GH Live.', 'Bronze'),  # 15
    ('Whoa!', 'Finish every song in Quickplay or GH Live on any difficulty with 5 stars.', 'Gold'),  # 16
    ('Rising Star', 'Get 150 stars in GHTV by playing songs.', 'Bronze'),  # 17
    ('All The Notes', 'Hit each type of note in a single song in GHTV.', 'Bronze'),  # 18
    ('Session Musician', 'Play with 8 different bands in GH Live.', 'Bronze'),  # 19
    ('Ooooh, You Fancy', 'Get 100% in 4 songs on Expert difficulty in GH Live.', 'Silver'),  # 20
    ('Touch Of Style', 'Acquire 4 Player Cards in GHTV.', 'Bronze'),  # 21
    ('Animal Magnetism', 'Log in to GHTV 5 days in a row.', 'Bronze'),  # 22
    ('Excellent Constitution', 'Finish in the top 5 of 18 consecutive On Demand songs in GHTV.', 'Bronze'),  # 23
    ("Let's Do This!", 'Complete the GH Live tutorial.', 'Bronze'),  # 24
    ('Just A Bit', 'Score over 30,000 points in 50 songs in GHTV.', 'Bronze'),  # 25
    ('Loitering', 'Connect to GHTV 10 days in a row.', 'Bronze'),  # 26
    ('Power Crazy!', 'Use a Hero Power 6 times during a song in GHTV.', 'Silver'),  # 27
    ('Hero Power-ful', 'Use Hero Powers 1,000 times in GHTV.', 'Bronze'),  # 28
    ('Meet The Bands!', 'Unlock and watch all of the band profile videos in both festival galleries in GH Live.', 'Bronze'),  # 29
    ('Bling Of The Road', 'Equip 5 different Highways in GHTV.', 'Bronze'),  # 30
    ('Rock Expert', 'Get 50 stars in GHTV by playing songs on Expert difficulty.', 'Bronze'),  # 31
    ('Specialist Shredder', 'Fully upgrade a skill on your guitar in GHTV.', 'Silver'),  # 32
    ('Power Hour', 'Play GHTV for 60 minutes without changing the channel.', 'Bronze'),  # 33
    ('Nailed It', 'Score over 500,000 points in a song in GHTV.', 'Bronze'),  # 34
    ('Stars Upon Stars Upon Stars', 'Get 5 Stars in 50 songs in GHTV on any difficulty.', 'Bronze'),  # 35
    ("3rd Place Ain't Bad", 'Place 3rd or above 30 times in the free GHTV channels.', 'Bronze'),  # 36
    ('No Need For A Hero', 'Score 420,000 points or more in a song without using a Hero Power in GHTV.', 'Silver'),  # 37
    ('Consistent', 'Get a total of 25 Stars in 5 consecutive songs on free GHTV channels.', 'Bronze'),  # 38
    ('I Have The Power', 'Use 5 unique Hero Powers in GHTV.', 'Bronze'),  # 39
    ('Really, Really Good', 'Give an "Awesome" performance in all sets in GH Live on Regular difficulty or harder.', 'Silver'),  # 40
    ('The Great Compromise', 'Get a 50 note streak 50 times while playing GHTV.', 'Bronze'),  # 41
    ('You Rocked The Block', 'Complete all of the Rock the Block stages in GH Live on any difficulty.', 'Silver'),  # 42
    ('Sound Dialed In', 'Complete all of the SoundDial stages in GH Live on any difficulty.', 'Silver'),  # 43
    ('Now With More Awesome', 'Complete 7 sets in GH Live, each with an overall "Awesome" rating, on any difficulty.', 'Silver'),  # 44
    ('Piece Of Cake', 'Complete all of GH Live on any difficulty.', 'Bronze'),  # 45
    ('Day', 'Play GHTV for 24 hours.', 'Bronze'),  # 46
    ('All The Points!', 'Score 50,000,000 points in GHTV.', 'Bronze'),  # 47
    ('Strumillionaire', 'Strum 1,000,000 times in GHTV.', 'Silver'),  # 48
    ('Amazing!', 'Earn a 1,000 note streak in GHTV.', 'Silver'),  # 49
)


def read_trp_icons(path):
    """{trophy id: PNG bytes} of a trophy00.trp (big-endian header: magic DC A2 4D 00, the file
    count at 0x10, the entry size at 0x14; entries from 0x60: a 32-byte name, offset, size)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return {}
    if len(data) < 0x60 or data[:4] != bytes((0xDC, 0xA2, 0x4D, 0x00)):
        return {}
    count, size = struct.unpack_from('>II', data, 0x10)
    if not 0x30 <= size <= 0x100:
        return {}
    icons = {}
    for i in range(count):
        entry = 0x60 + i * size
        if entry + 0x30 > len(data):
            break
        name = data[entry:entry + 32].split(b'\0')[0].decode('ascii', 'replace')
        offset, length = struct.unpack_from('>QQ', data, entry + 0x20)
        if name.startswith('TROP') and name.endswith('.PNG') and name[4:7].isdigit() and offset + length <= len(data):
            icons[int(name[4:7])] = data[offset:offset + length]
    return icons


def read_unlocked(path):
    """The ids the runtime saved as unlocked (trophies.dat: four little-endian 32-bit masks)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return set()
    if len(data) < 16:
        return set()
    masks = struct.unpack_from('<4I', data)
    return {i for i in range(128) if masks[i // 32] >> (i % 32) & 1}
