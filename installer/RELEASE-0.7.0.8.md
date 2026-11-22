## EXPERIMENTAL - companies rewritten

Test build based on 0.7.0.7, for Windows. Everyone in the session must update to 0.7.0.8. Only the **separate companies** mode changes; co-op games play as in 0.7.0.7.

### Changes

- **Companies belong to players, not to lobby slots.** A company is tied to its players' Steam IDs (the lobby name for players without one). A save loaded with a different host keeps everyone in their own company, with their own money and vehicles. Before, a host swap could swap companies: the host started at 0 and a friend got the starting money, and vehicles changed owner.
- **Every machine decides alike.** Which company a command belongs to is decided when it is stamped, from state every machine shares. New companies start with the same loan as the first one.
- **The COMPANIES tab is rebuilt:** your company and its settings on top, the list of companies below, one panel for the selected company. Fewer buttons.
- **Company colours:** choose one from a palette. Each colour belongs to one company. Painting vehicles in the company colour is now a switch per company. It is on by default, as before.
- **Delete a company:** choose the company that takes over its vehicles, lines, buildings, money and loan, or **Nobody**. Nobody sells and removes everything. It shows a warning first, and the red DELETE EVERYTHING button must be clicked twice.
- **One headquarters per company.** Only the owner can change a building. A change to another company's building is undone and not sent.
- **Old saves are migrated.** The host keeps their company, and every other player gets back the company of their lobby slot. Passwords stay valid.
- **The menu showed every player as still loading** in companies mode, so company changes were refused ("wait until ... has loaded in"). Fixed.

### How to test

1. Close the game. In the launcher turn on **Experimental** releases, then **Update & play**. Everyone in the session does the same.
2. Start a new game in separate companies mode. Create, rename, recolour, switch and delete companies (with a taker, and once with Nobody). Build a headquarters for two companies.
3. Save, and load the save with the other player hosting. Everyone must be in their own company with their own money.
4. Load a save made with 0.7.0.7 or older and check the companies, money and vehicles.
5. Keep both players' logs from any problem: Multiplayer window, **Open logs**.

### Known limitations

- **Windows only.** No native Linux or Proton package for this test build: Linux players stay on 0.7.0.7 and cannot join 0.7.0.8.
- Keep a copy of saves you care about. A save made with 0.7.0.8 is not meant to be loaded by 0.7.0.7.
- Tested offline and on one computer with two games. Not yet played between two computers over the Internet.
- When a company is deleted with Nobody, anything the game refuses to remove, such as a station another company still serves, goes to the deleting player's company.

### Validation

A multi-machine model of the company registry, including the reported host swap and old-save migration (company_registry_test). Delete with a taker and with Nobody, headquarters, colours and starting loan (company_rules_test). The COMPANIES tab (company_gui_test). The loading gate, the palette in all four places, the construction scan (con_slice_test), and the version gate. Create, switch and delete were also played on one computer with two games.
