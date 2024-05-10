Translated to English by ZaRR.
Notes: 
	Translatio of programm was done manually, but big texts such as license and readme were google translated.
	Shablons = schemes
	Some strings 'OK' might be '??'...
	
	
	
	
	
Resource editor for the Cursed Lands.
                            Version 1.8.2

                            Crafted by Sagrer
                  Copyright © 2002-2007 Gipat Group

            The program is distributed under the conditions
       Gipat Group's opened EI-editor-utility license ver. 1.0.
____________________________________________________________________

"Cursed Lands" Copyright 2000 Nival Interactive.
Copyright 2000 AOZT 1C. All rights reserved.

Cursed Lands is a trademark of Nival Interactive.

____________________________________________________________________

                                        ATTENTION!!!!!!!!!!!!!!!

This version of the program contains a known error in the editor's code.
acks.db. We are aware of the error, but since the immediate
we do not need to work with acks.db - its fix is ??postponed "for later".

Do not try to edit this part of the database with this version of the program!
Almost certainly acks.db will become unusable after such editing.
- the editor will still open it but the correct information is in this
file will be lost, respectively, and the game will most likely simply crash when
trying to use database.res with this file ... and if it does not crash then glitches
will be terrible! %).
____________________________________________________________________

                               Referral of the author to users.

If, after trying to use the program, you ask the author to go
drink yada - you will be absolutely right%). Then, back in 2001-2002, this is
was my first program really doing something%). Well, if you don't count
any HelloWorld or Pascal problems in computer science. Even files
that version read through Readln and wrote through Writeln%))). Just like in some thread
"Pascal 3.0 for IBM PC"%))).

Yes, since then the code has been totally cleaned up several times, once in general most of
the insides were disassembled into separate incompatible pieces and assembled
again, but, alas, to write a good program, you need at least 1-2
rewrite from scratch. This program has not yet been rewritten since those
since.

The only thing for which I am not too ashamed is the text editor that has been re-created
rewritten relatively recently. The database editor expects the same complete
replacements - something that is capable of editing the base, but adding something more
complex (like autobalancing) on ??that code is unrealistic. It's all tied there
to the interface. The code is terrible there, yes%).

The only thing that makes the program unique is that it is capable of almost directly
to work with the binary base of the PP and not with its textual sources.

In general, if anything - I warned you%).
____________________________________________________________________

So what is there (like content):


1. Nonsense.

2. About this version.

3. What the program can do.

4. Using a resbuild.

5. Editing of texts.

6. Editing bases.

7. A ballad about a changelog.

8. Coordinates of the authorof.
____________________________________________________________________

1. Brawl

  Actually, here I just want to say that the file (readme), like the program itself
intended exclusively "for their own" - that is, for use
members of the Gipat Group, that is, people who at least superficially
familiar with the internal structure of PZ, and with gipat.org-ovsky PZ-shny
jargon (if you do not know what PZ is, then you are definitely not familiar;)).
Why am I saying this - I will not explain what, for example, "resbuild" is,
where to get it and how to use it, and why you need it at all. Not for this
readme. Otherwise, if something is not clear, you can ask on the forum.
___________________________________________________________________

2. Request for this version.
  In this section I will pay attention to what you should pay attention to
when switching from a previous version to this one.

Version 1.8.2

Compared to 1.8.1, there is nothing fundamentally new except that
the source of the program and the tracker are now published.
___________________________________________________________________

3. What the program can do.

  So, he can:

1) BOOK OUT!
2) Corrupt the PZ files

Although I try to reduce the impact of the first two points
but still - make backups.

3) Edit all sorts of different text files, well, RTF at the same time (although
and xs how safe it is and does not spoil the format).

4) Edit the contents of database.res (some of these files are in
databaselmp.res).

5) Use resbuild without command line (choose which file to unpack
or which folder to pack). + optimization :).
____________________________________________________________________

4. Using a resbuild.

Basically, you can use EiEdit as a wrapper for ResBuild.
There are 2 ways - the first is described here, the second - see below in the description
text editor.

So, resbuild. There is a button on the toolbar with "RES" drawn on it,
it is duplicated in the FILE menu by the item "Packing \ unpacking resources ...". If there
press, then a window will come out. There is something like a table in the window.itsu, only instead of cells
buttons. Buttons located on the same line perform any action
over the cut whose name is written at the beginning of the line. The action that performs
button is written in the header of the button column. In general, it should be clear there
in appearance. Packing - packs, unpacking - oddly enough, unpacks;).
When unpacking, all files are unpacked into a folder with the file name, if the file has
there is an extension - then the symbol "_" is assigned to the folder after the file name, and after
him - symbols from the extension. If the file does not have an extension, it will be assigned to the folder
"_noext". The folder is located in the same place as the file with the cut. If before that in a folder,
where the files are stored, then everything is pre-erased there (warning
the program issues). When packing, the files for packing are taken from a folder named
obtained as described above. If the button is faded, then there is nothing
unpack \ pack.

BUT all these buttons work only if the names of the resource files are written in
program settings ("paths to resource files").

Only 2 buttons always work, which say "other". One is for packing,
the other is for unpacking (strange, isn't it?). When you click, a window pops up where necessary
choose what to unpack or pack. If you unpack it, you will need to
open the cut file in the window. If you pack, then select the folder with the files to be packed.
After that, a window will appear in which you can specify exactly what and how to rebuild,
if the proposed program does not like something.

There is also an optimization button - by clicking, you need to select a file that
optimize.
There is also a checkbox - if you enable it, then after packing the "standard"
resources will be optimized.
__________________________________________________________________________

5. Editing text.

So. The editor is called by the corresponding button on the toolbar, or from the FILE menu.

Further - what he can do.
1) Open regular text files
2) Open * .rtf files.
3) Correctly save the whole thing.
4) Enable / disable word wrap
5) Built-in navigation on the computer disk - a kind of freak file manager.
6) Built-in ability to unpack \ pack resbuild archives
7) You can delete files \ folders directly in the editor.

How it works.
In the editor window there is a field - a list of files / folders in the current folder. Icons there
not quite standard, but in appearance it should be clear what they mean.
Corresponds to the file system element type - file (and its type) \ folder \ link \ unknown.
The review starts from the path specified in the text editor settings.
These settings are located in the menu PARAMETERS \ Parameters for the text editor.

How to navigate folders .... Norton Commander or his clones
did you use it? ;). So - we poke around with a rodent 2 times in a folder - we get there. We poke
by an element with two dots instead of a name - we get one level higher.
And yet - at the top there is an input field for the path - you can also enter the path there and press enter
or a button with an arrow next to the input field - it is convenient, for example, to copy the path from
explorer windows or somewhere else - in principle, this is the fastest way to move
in case when somewhere this path is already open.
Also, it can delete files / folders. Highlight what you want and press DEL. Proga essno
will ask for confirmation, because the basket is not used.

If you make 2 clicks on the resbuild archive, you will be prompted to unpack this resource,
and after unpacking - the program will automatically fit into the folder with unpacked files.

Further - on the right - a field for the text. A regular text editor like "BLAKNOT" :). Everybody there
standard. Below it is a checkbox to enable / disable line wrapping - maybe
need to understand where the paragraph ends.

Even lower is the switch between RTF-TXT. In general, it switches automatically
depending on the file type. But if you suddenly want to save your usual
textbook as rtf - you can manually change the switch and click on the save button.
Similarly, you can turn rtf into a regular text editor.

To the right of the switch is the save button. Have questions? ;). I'll just say that
the file is saved to the current folder (the one displayed in the left field). And the name
- what is written above the editor window is used. If you change the name, and \ or
go to another folder (or all together) and click on the save file - then a new file will be created
(or the existing one will be overwritten, but different - depending on what was entered and where
passed) - sometimes a convenient way to quickly "copy" a file.

As already said - at the top of the editor is a line under the file name. By the way, even if never
the file has not been opened yet - you can enter the file name and text into the editor and save
- a file will be created.

So. Well, further - under the list of files - a drop-down list for filters. Well, in short the mask is.
File masks. Asterisk (*) - any number of any characters, question mark (?) - any 1
symbol. I hope everyone is also familiar with this case. In general, filters are used to
view in the list of files only those that match the mask. The filters are different there,
I have already done them for all kinds of text resources in the PZ. Anyone can be killedfilter
acc. the button to the right of the filter list. And you can add a new filter - also a button
for this there is, in the same place. In the window that appears - enter the mask itself and the description of the filter
(any text).

Farther. Checkbox "open with 1 click (selection)". In general, text files - (a
all unknown files are considered text files;)) if
turn on the checkbox - then such files will be opened with 1 click (you need to select it).
You can simply move the selection with the arrows on the keyboard - the files will open. This
can be used to quickly view a bunch of files.

Below the above-described checkbox, another checkbox is set - if it is enabled, then text files
larger than the size entered in the input field to the right of the checkbox - they will not open.

Below the list of filters are the buttons for packing and unpacking. If you select a folder and press
zip - you will be prompted to zip the folder into the resbuild archive. Well, the decay button
forging works similarly to 2 clicks on the resbuild archive. Only essno must first
highlight this file.

In general, in its current state, the editor should be capable of without using
other software to edit outro.res and * .mq-shki :). Well, the contents of some
texts.res naturally.

By the way about editing texts.res - in the original game and in the vast majority
mods absolutely all texts are in one file texts.res. Navigate there
quite difficult. In mods for a starter, you can of course scatter content
on several resources, but such things do not always occur in any case.
So - in order not to scroll through the entire huge list - there are filters.
A drop-down list with these filters is located below the list of files. Filters
are ordinary masks that are often used when working with
file system, for example, when allocating files by mask in Far. For those, who
I lived my life in an armored vehicle and is not aware of how these masks work -
quick reference - * replaces any number of any characters,? replaces one
any character - total examples:
* .txt - means all files with txt extension,
* test * .txt - all files in whose name somewhere in the middle there is test and extension
which txt,
??? test.txt - all files ending with test.txt with 3 more characters in front.
* - ALL files in general, including those without the extension
*. * - all files that have an extension
etc.

You can add new filters using the corresponding buttons. Delete - the same way.

___________________________________________________________________________
6. Editing bases

  Firstly, you need to show the program where the * .res -files of the databases are. This can be done either
through the settings window for the paths to all resources of the program, or through the Options \ Options menu
for bases.

In the upper 2 fields, you can manually or with the browse button write paths to unpacked
resbuild databases (database.res and databaselmp.res). In the same window, in fields 3 and 4 - show
folders where the "database sources" will be located - folders with files used by the editor,
unpacked (more precisely, finely chopped) base. This folder should not contain folders with
the names acks, items, levers, perks, prints, quests, spells, units - such names will be for folders
as part of the source, it is better if it is an empty folder at all. By default, the program in general
writes there the path to a special folder for this, which lies in the program folder.

  Now you need to unpack the databases. Databases \ Sources menu ...
  There are 2 sections in the window - for a single and for a multi base. For each 6 buttons.
  [Unpack by resbuild] - cut * .res base file. Pack up - essno reverse
process (unpacked files appear in a folder with the name of the base file in the same folder
where and * .res is the base file) - these buttons are simply unpacked with a resbuild.
  [Generate source] - this is already sobsno "unpacking" the split-built databases - getting
"source", which is written to the previously specified folder (for the source).
  [Generate databases] - the database files are obtained from the "source" (for example, units.udb), which
are written to where the rezbuild database was unpacked. After which they can be again
zarezbuild in * .res file with the appropriate button.
  [Pack with resbuild] - in the sense - packs already generated databases. Then it optimizes.

  [Full unpacking] - Unpacks the base with a resbuild, and immediately makes
source (like 2 buttons in one).
  [Full packing] - the same. Reverse action only. 2 buttons in one.

  In principle, the last 2 types of buttons are enough for work. The rest remained from
past, did not clean up - suddenly they will need ...

  Editing itself - the "source" is being edited. I recommend getting into the folder with
source and study (with the involvement of a HEX editor) in order to better understand what it is
such, but it is not necessary.
In short, each line in the Nivalov textbook (well, the one that "utility for
creating mods in multiplayer "or something like that) - and so, each line in that textbook
corresponds to 1 file in the "source" of this program. In fact, these files are the result of fine slicing
sobsno base (well, there is also the structure of the base itself removed the one that is not needed), sowould be at 1
the file had 1 line ("scientific";) - records) of the base.

  All editing windows have:
  1) two drop-down lists - the right one with the names of the source files being edited, and the left one -
with "masks". "Mask" - this is how I called the first field of database records, which is of text type everywhere,
and is always unique - usually this field is named Name. The stump is clear, which is easier to find by name
the desired entry than the file name like "0000057.zap". Both of these lists are "in sync"
- the desired file can be selected both there and there, the state of the other list will change automatically.
  2) A table with record fields - see below for what fields are there.
  3) Buttons:
  [Template] - create a new template or overwrite the old one. See below for templates.
  [Save] - save the entry.
  [Close] - close the window, WITHOUT saving.
  [Create] - create a record using a template.
  [Delete] - delete the entry.

  About field types:
  1) TEXT - any text. Be more attentive to the register, there are suspicions that the Nival
tools for bases correct words in the wrong case (for example, make all letters small),
I'm definitely not sure about this in all places, so my texts are saved as you entered them.
In general, see by analogy.
  2) UnsLong. - an integer number. Only numbers and a minus sign.
  3) FLOAT. - fractional number. You can write without a comma if it is without a fractional part. Visually from
UnsLong can be distinguished by the fact that when opening a record, all FLOAT numbers have a comma, even
if the fractional part is zero (for example, 1.0). You can write - only numbers, a minus sign and a comma.
  4) ShopInfo - a comma-separated list of store numbers where the item is sold. Commas
it is impossible at the beginning and at the end of the line, they must be written only between the numbers.
  5) LIST - a list of string (text) values. Separated by commas. Write by the same rules
as ShopInfo.
  6) Unknown with a button. Those unknown (Unknown *) fields for which the format is unclear are assigned
button (visible when the field is selected). By clicking on the button, you can save the internal representation
fields in a file, or load from a file.
  7) Other fields - they are different, there are a lot of them ... what is above - I mentioned the main ones.
They usually look like ordinary ones and there is no difference in editing them. And if there is a difference,
then the program will check, before saving, what was written there, and if something is wrong -
will display a dialog with an error message and brief instructions on what you can and cannot write in
this field.
  8) "Fields" with a button, but not "unknown" - usually a button is placed if for editing a field
you need to call an additional dialog (for example, in Acks.db), or if pressing the button allows
edit the field faster. For example, many ID fields (ID is in the field name) are
integer numbers, but a word can correspond to certain field values ??(for example
00 = sword, 01 = ax) ... In general, such fields look something like this (do not count squares;)):

[1 | AX]

That is, there is a number, then a straight line, then a word. In principle, in such a field you can write a number "by hand" (the program will only accept what is written from the beginning of the line, everything after the first space (and the space itself) is ignored). But you can click on the button in this field and select the desired number from the list (you can see what each of the numbers means).

  About templates. Templates are blank records. Each type of base (and each of its sections)
 they are different and are contained in different folders. To create a template, you need to open (or create)
record, on the basis of which the template will be made, set the required values ??and click [Template].
In the window that appears, either enter a new name, or select an existing template from the list.
Well, click OK. Everything.
  To create a record, after clicking [Create], enter the correct name of the record (so that it was
unique), and then select the desired template. Everything.
___________________________________________________________

7. A ballad about a changelog.

Once upon a time the 7th item contained something like a "list of innovations". Now
about such a list is in the ChangeLog.txt file and can be seen when
also when starting the installation. It is removed from here. For nefig.

___________________________________________________________

8. Coordinates of the authorof.

The utility was developed by the Gipat Group team who sometimes live on
address www.GipatGroup.org. There is a forum where you can discuss
program.

The actual development of the program is carried out at
http://svn.gipat.org/trac/EiEdit - here you can check the status
development, look at further plans, lists of bugs, download
the program itself and the source (including the current version from svn), etc., etc.

If you find a bug in the program, go to the tracker, look for
bugs - have you already reported such an error. If there are no messages -
create a New Ticket in which you describe in detail the nature of the error,
if the program generates some kind of error message - preferably
attach a screenshot from this post. In the settings of the created
ticket do not forget to indicate that the ticket type is a bug, that the component is also
bar, indicate the version number of the program you are using.

If you want to suggest something to improve in the program - similarly
create a new ticket specifying the ticket type - "offer", component - select
the component to which you think the improvement belongs (you can
select "other") and describe the proposed improvement. Version - choose your
the current one you are using. Milestone - select "offers".

If you yourself made some improvement in the source - make a patch from
the current version of svn - and send the patch again in the form of a Ticket.
Ticket type - select a patch, component - patches, attach the patch file itself
to the ticket, describe the improvement you made, be sure to indicate in the text
the revision number to which you want to apply the patch.

If you want to take part in the development - let us know about your intention
developers (for example via the forum), then select any unoccupied task,
implement it and send it as a patch - if you can prove that you can
write sane code - get constant write access to svn.

___________________________________________________________

The coordinates are authorof.

The utility was developed by the Gipat Group team who sometimes live on
address www.GipatGroup.org. There is a forum where you can discuss
program.

The actual development of the program is carried out at
http://svn.gipat.org/trac/EiEdit - here you can check the status
development, look at further plans, lists of bugs, download
the program itself and the source (including the current version from svn), etc., etc.