// ============================================================
// Component C
//
// Directed edges:
//
// e1:   1 -> 2
// e2:   3 -> 1
// e3:   1 -> 4
// e4:   3 -> 2
// e5:   4 -> 3
// e6:   2 -> 5
// e7:   3 -> 6
// e8:   3 -> 7
// e9:   6 -> 7
// e10:  7 -> 8
// e11:  8 -> 5
//
// Under undirected dynamic-connectivity semantics,
// nodes {1,2,3,4,5,6,7,8} form one connected component.
// ============================================================


// ------------------------------------------------------------
// Schema
// ------------------------------------------------------------

CREATE NODE TABLE Person(
    id INT64,
    PRIMARY KEY(id)
);

CREATE REL TABLE Knows(
    FROM Person TO Person,
    eid INT64
);


// ------------------------------------------------------------
// Nodes
// ------------------------------------------------------------

CREATE (:Person {id: 1});
CREATE (:Person {id: 2});
CREATE (:Person {id: 3});
CREATE (:Person {id: 4});
CREATE (:Person {id: 5});
CREATE (:Person {id: 6});
CREATE (:Person {id: 7});
CREATE (:Person {id: 8});


// ------------------------------------------------------------
// Directed relationships
// ------------------------------------------------------------

// e1: 1 -> 2
MATCH (a:Person {id: 1}), (b:Person {id: 2})
CREATE (a)-[:Knows {eid: 1}]->(b);

// e2: 3 -> 1
MATCH (a:Person {id: 3}), (b:Person {id: 1})
CREATE (a)-[:Knows {eid: 2}]->(b);

// e3: 1 -> 4
MATCH (a:Person {id: 1}), (b:Person {id: 4})
CREATE (a)-[:Knows {eid: 3}]->(b);

// e4: 3 -> 2
MATCH (a:Person {id: 3}), (b:Person {id: 2})
CREATE (a)-[:Knows {eid: 4}]->(b);

// e5: 4 -> 3
MATCH (a:Person {id: 4}), (b:Person {id: 3})
CREATE (a)-[:Knows {eid: 5}]->(b);

// e6: 2 -> 5
MATCH (a:Person {id: 2}), (b:Person {id: 5})
CREATE (a)-[:Knows {eid: 6}]->(b);

// e7: 3 -> 6
MATCH (a:Person {id: 3}), (b:Person {id: 6})
CREATE (a)-[:Knows {eid: 7}]->(b);

// e8: 3 -> 7
MATCH (a:Person {id: 3}), (b:Person {id: 7})
CREATE (a)-[:Knows {eid: 8}]->(b);

// e9: 6 -> 7
MATCH (a:Person {id: 6}), (b:Person {id: 7})
CREATE (a)-[:Knows {eid: 9}]->(b);

// e10: 7 -> 8
MATCH (a:Person {id: 7}), (b:Person {id: 8})
CREATE (a)-[:Knows {eid: 10}]->(b);

// e11: 8 -> 5
MATCH (a:Person {id: 8}), (b:Person {id: 5})
CREATE (a)-[:Knows {eid: 11}]->(b);